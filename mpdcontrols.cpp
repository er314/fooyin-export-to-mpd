#include "mpdcontrols.h"

#include <core/library/libraryinfo.h>
#include <core/library/musiclibrary.h>
#include <core/playlist/playlisthandler.h>
#include <core/playlist/playlist.h>
#include <gui/playlist/currentplaylistcontroller.h>
#include <gui/trackselectioncontroller.h>

#include <QMessageBox>
#include <QCheckBox>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QPushButton>
#include <QtConcurrent/QtConcurrentRun>

#include <QAction>
#include <QActionGroup>
#include <QMenu>
#include <QStyle>
#include <QToolButton>
#include <QSettings>

#include <algorithm>

#include <mpd/client.h>

namespace {

QString mpdError(mpd_connection* connection)
{
    const char* message = mpd_connection_get_error_message(connection);
    return message ? QString::fromUtf8(message)
                   : QStringLiteral("Unknown MPD error");
}

QList<MpdAudioOutput> readMpdOutputs(
    mpd_connection* connection,
    QString* error)
{
    QList<MpdAudioOutput> result;

    if (!mpd_send_outputs(connection)) {
        *error = mpdError(connection);
        return result;
    }

    while (auto* output = mpd_recv_output(connection)) {
        const char* name = mpd_output_get_name(output);
        const char* plugin = mpd_output_get_plugin(output);

        result.append({
            mpd_output_get_id(output),
            name ? QString::fromUtf8(name) : QString{},
            plugin ? QString::fromUtf8(plugin) : QString{},
            mpd_output_get_enabled(output)
        });

        mpd_output_free(output);
    }

    if (!mpd_response_finish(connection)) {
        *error = mpdError(connection);
        result.clear();
        return result;
    }

    // Keep MPD's original order within each category, but put ALSA outputs
    // after other plugins and HTTPD outputs after ALSA outputs.
    const auto outputPriority = [](const MpdAudioOutput& output) {
        if (output.plugin == QStringLiteral("httpd")) {
            return 2;
        }
        if (output.plugin == QStringLiteral("alsa")) {
            return 1;
        }
        return 0;
    };

    std::stable_sort(
        result.begin(), result.end(),
        [&outputPriority](const MpdAudioOutput& lhs,
                          const MpdAudioOutput& rhs) {
            return outputPriority(lhs) < outputPriority(rhs);
        });

    return result;
}

void restoreOrChooseOutput(const QList<MpdAudioOutput>& outputs,
                           int* selectedOutputId,
                           QString* selectedOutputName,
                           QString* selectedOutputPlugin)
{
    if (outputs.isEmpty()) {
        // Keep the saved name/plugin so the selection can be restored if
        // outputs become available again later.
        *selectedOutputId = -1;
    } else {
        auto selected = outputs.cend();

        // Prefer the saved plugin/name identity. MPD output IDs can change
        // when the server configuration is edited or outputs are recreated.
        if (!selectedOutputName->isEmpty() || !selectedOutputPlugin->isEmpty()) {
            selected = std::find_if(
                outputs.cbegin(), outputs.cend(),
                [selectedOutputId, selectedOutputName, selectedOutputPlugin](
                    const MpdAudioOutput& output) {
                    return static_cast<int>(output.id) == *selectedOutputId
                           && output.name == *selectedOutputName
                           && output.plugin == *selectedOutputPlugin;
                });

            if (selected == outputs.cend()) {
                selected = std::find_if(
                    outputs.cbegin(), outputs.cend(),
                    [selectedOutputName, selectedOutputPlugin](
                        const MpdAudioOutput& output) {
                        return output.name == *selectedOutputName
                               && output.plugin == *selectedOutputPlugin;
                    });
            }
        } else {
            selected = std::find_if(
                outputs.cbegin(), outputs.cend(),
                [selectedOutputId](const MpdAudioOutput& output) {
                    return static_cast<int>(output.id) == *selectedOutputId;
                });
        }

        if (selected == outputs.cend()) {
            selected = outputs.cbegin();
        }

        *selectedOutputId = static_cast<int>(selected->id);
        *selectedOutputName = selected->name;
        *selectedOutputPlugin = selected->plugin;
    }

    QSettings settings;
    settings.beginGroup(QStringLiteral("MpdControls"));
    settings.setValue(QStringLiteral("selectedOutputId"), *selectedOutputId);
    settings.setValue(QStringLiteral("selectedOutputName"), *selectedOutputName);
    settings.setValue(QStringLiteral("selectedOutputPlugin"), *selectedOutputPlugin);
    settings.endGroup();
}

bool enableSelectedOutput(
    mpd_connection* connection,
    int* selectedOutputId,
    QString* selectedOutputName,
    QString* selectedOutputPlugin,
    QString* error)
{
    const auto outputs = readMpdOutputs(connection, error);

    if (!error->isEmpty()) {
        return false;
    }

    if (outputs.isEmpty()) {
        *error = QStringLiteral("MPD has no available audio outputs");
        return false;
    }

    restoreOrChooseOutput(
        outputs, selectedOutputId, selectedOutputName, selectedOutputPlugin);

    // Disable all other outputs.
    for (const auto& output : outputs) {
        if (static_cast<int>(output.id) == *selectedOutputId) {
            continue;
        }

        if (output.enabled
            && !mpd_run_disable_output(connection, output.id)) {
            *error = mpdError(connection);
            return false;
        }
    }

    // Enable the selected output.
    const auto selectedOutput = std::find_if(
        outputs.cbegin(),
        outputs.cend(),
        [selectedOutputId](const MpdAudioOutput& output) {
            return static_cast<int>(output.id) == *selectedOutputId;
        });

    if (selectedOutput != outputs.cend() && !selectedOutput->enabled) {
        if (!mpd_run_enable_output(connection, selectedOutput->id)) {
            *error = mpdError(connection);
            return false;
        }
    }

    return true;
}

struct MpdConfig
{
    QString host;
    unsigned port{6600};
    QString password;
};

MpdConfig readMpdConfig()
{
    MpdConfig config;
    config.host = qEnvironmentVariable("FOOYIN_MPD_HOST", "127.0.0.1");
    config.password = qEnvironmentVariable("FOOYIN_MPD_PASSWORD");

    bool ok = false;
    const uint port =
        qEnvironmentVariable("FOOYIN_MPD_PORT", "6600").toUInt(&ok);

    if (ok && port > 0 && port <= 65535) {
        config.port = port;
    }

    return config;
}

mpd_connection* openMpdConnection(QString* error)
{
    const MpdConfig config = readMpdConfig();
    const QByteArray host = config.host.toUtf8();

    auto* connection =
        mpd_connection_new(host.constData(), config.port, 5000);

    if (!connection) {
        *error = QStringLiteral("Could not allocate an MPD connection");
        return nullptr;
    }

    if (mpd_connection_get_error(connection) != MPD_ERROR_SUCCESS) {
        *error = QStringLiteral("MPD connection failed: %1")
                     .arg(mpdError(connection));

        mpd_connection_free(connection);
        return nullptr;
    }

    if (!config.password.isEmpty()) {
        const QByteArray password = config.password.toUtf8();

        if (!mpd_run_password(connection, password.constData())) {
            *error = QStringLiteral("MPD authentication failed: %1")
                         .arg(mpdError(connection));

            mpd_connection_free(connection);
            return nullptr;
        }
    }

    return connection;
}

QStringList makeMpdUris(const Fooyin::TrackList& tracks,
                        Fooyin::MusicLibrary* library,
                        QString* error)
{
    if (!library) {
        *error = QStringLiteral("Fooyin music library is unavailable");
        return {};
    }

    QStringList uris;
    uris.reserve(static_cast<qsizetype>(tracks.size()));

    for (const Fooyin::Track& track : tracks) {
        if (track.isRemote() || track.isVirtual() || track.isInArchive()
            || track.hasCue() || track.isBoundedSegment()
            || track.subsong() != 0) {
            *error = QStringLiteral(
                "This playlist contains a remote, archive, cue, chapter, "
                "or subsong entry that cannot be mapped to an MPD file URI.");
            return {};
        }

        const QString filepath =
            QDir::cleanPath(QFileInfo(track.filepath()).absoluteFilePath());

        const auto libraryInfo = library->libraryForPath(filepath);
        if (!libraryInfo) {
            *error = QStringLiteral(
                "Fooyin has no configured library root for: %1").arg(filepath);
            return {};
        }

        const QString rootPath =
            QDir::cleanPath(QFileInfo(libraryInfo->path).absoluteFilePath());
        const QString relativePath = QDir(rootPath).relativeFilePath(filepath);

        // Guard against paths outside the configured root.
        if (relativePath == QStringLiteral("..")
            || relativePath.startsWith(QStringLiteral("../"))
            || QDir::isAbsolutePath(relativePath)) {
            *error = QStringLiteral(
                "Track is outside its fooyin library root: %1").arg(filepath);
            return {};
        }

        // Send a plain UTF-8 MPD database path, not an absolute file:// URL.
        uris.push_back(relativePath);
    }

    return uris;
}

QString runMpdOperation(const QStringList& uris,
                        bool toStoredPlaylist,
                        bool clearFirst,
                        const MpdConfig& config)
{
    const QByteArray host = config.host.toUtf8();
    mpd_connection* connection =
        mpd_connection_new(host.constData(), config.port, 5000);

    if (!connection) {
        return QStringLiteral("Could not allocate an MPD connection");
    }

    if (mpd_connection_get_error(connection) != MPD_ERROR_SUCCESS) {
        const QString error = mpdError(connection);
        mpd_connection_free(connection);
        return QStringLiteral("MPD connection failed: %1").arg(error);
    }

    if (!config.password.isEmpty()) {
        const QByteArray password = config.password.toUtf8();
        if (!mpd_run_password(connection, password.constData())) {
            const QString error = mpdError(connection);
            mpd_connection_free(connection);
            return QStringLiteral("MPD authentication failed: %1").arg(error);
        }
    }

    const QByteArray playlistName = QByteArrayLiteral("Filtered playlist");
    const QString clearTarget = toStoredPlaylist
        ? QStringLiteral("MPD playlist “Filtered playlist”")
        : QStringLiteral("MPD queue");

    bool wasCleared = false;

    const auto fail = [&](const QString& message) {
        const QString result = wasCleared
            ? QStringLiteral("%1 was cleared, but %2").arg(clearTarget, message)
            : message;
        mpd_connection_free(connection);
        return result;
    };

    if (toStoredPlaylist && clearFirst) {
        // Create the stored playlist if needed, then empty it.
        const QByteArray firstUri = uris.constFirst().toUtf8();
        if (!mpd_run_playlist_add(connection, playlistName.constData(),
                                  firstUri.constData())) {
            const QString error = mpdError(connection);
            mpd_connection_free(connection);
            return QStringLiteral("Could not prepare MPD playlist: %1").arg(error);
        }

        if (!mpd_run_playlist_clear(connection, playlistName.constData())) {
            const QString error = mpdError(connection);
            mpd_connection_free(connection);
            return QStringLiteral("Could not clear MPD playlist: %1").arg(error);
        }

        wasCleared = true;
    } else if (!toStoredPlaylist && clearFirst) {
        if (!mpd_run_clear(connection)) {
            const QString error = mpdError(connection);
            mpd_connection_free(connection);
            return QStringLiteral("Could not clear MPD queue: %1").arg(error);
        }

        wasCleared = true;
    }

    for (const QString& uri : uris) {
        const QByteArray uriUtf8 = uri.toUtf8();

        const bool succeeded = toStoredPlaylist
            ? mpd_run_playlist_add(connection, playlistName.constData(),
                                   uriUtf8.constData())
            : mpd_run_add(connection, uriUtf8.constData());

        if (!succeeded) {
            const QString error = mpdError(connection);
            return fail(QStringLiteral("MPD add failed: %1").arg(error));
        }
    }

    mpd_connection_free(connection);

    const QString result = QStringLiteral("Added %1 track(s) to %2")
        .arg(static_cast<qulonglong>(uris.size()))
        .arg(toStoredPlaylist ? QStringLiteral("Filtered playlist")
                              : QStringLiteral("MPD queue"));

    return wasCleared
        ? QStringLiteral("%1 cleared; %2").arg(clearTarget, result)
        : result;
}

} // namespace

MpdControls::MpdControls(QObject* parent)
    : QObject{parent}
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("MpdControls"));
    m_selectedOutputId = settings.value(QStringLiteral("selectedOutputId"), -1).toInt();
    m_selectedOutputName = settings.value(QStringLiteral("selectedOutputName")).toString();
    m_selectedOutputPlugin = settings.value(QStringLiteral("selectedOutputPlugin")).toString();
    settings.endGroup();
}

void MpdControls::setFooyinServices(Fooyin::MusicLibrary* library)
{
    m_library = library;
}

void MpdControls::setGuiServices(
    Fooyin::CurrentPlaylistController* playlistSelection,
    Fooyin::TrackSelectionController* trackSelection)
{
    m_playlistSelection = playlistSelection;
    m_trackSelection = trackSelection;
}

void MpdControls::previous()
{
    QString error;
    auto* connection = openMpdConnection(&error);

    if (!connection) {
        emit statusChanged(error);
        return;
    }

    const bool ok = mpd_run_previous(connection);
    if (!ok) {
        error = mpdError(connection);
    }

    mpd_connection_free(connection);

    if (!ok) {
        emit statusChanged(error);
        return;
    }

    // MPD resumes playback when navigating to another track.
    // m_pauseMode represents the active-playback UI mode: true means
    // playback is active and the button must display the Pause icon.
    m_pauseMode = true;
    emit playbackModeChanged(true);
}

void MpdControls::next()
{
    QString error;
    auto* connection = openMpdConnection(&error);

    if (!connection) {
        emit statusChanged(error);
        return;
    }

    const bool ok = mpd_run_next(connection);
    if (!ok) {
        error = mpdError(connection);
    }

    mpd_connection_free(connection);

    if (!ok) {
        emit statusChanged(error);
        return;
    }

    // MPD resumes playback when navigating to another track.
    // m_pauseMode represents the active-playback UI mode: true means
    // playback is active and the button must display the Pause icon.
    m_pauseMode = true;
    emit playbackModeChanged(true);
}

void MpdControls::stopPlayback()
{
    QString error;
    auto* connection = openMpdConnection(&error);

    if (!connection) {
        emit statusChanged(error);
        return;
    }

    const bool ok = mpd_run_stop(connection);
    if (!ok) {
        error = mpdError(connection);
    }

    mpd_connection_free(connection);

    if (!ok) {
        emit statusChanged(error);
        return;
    }

    if (m_pauseMode) {
        m_pauseMode = false;
        emit playbackModeChanged(false);
    }
}

void MpdControls::togglePlayback()
{
    QString error;
    auto* connection = openMpdConnection(&error);

    if (!connection) {
        emit statusChanged(error);
        return;
    }

    bool ok = true;

    if (!m_pauseMode) {
        // PLAY mode: configure the selected output first.
        ok = enableSelectedOutput(
            connection,
            &m_selectedOutputId,
            &m_selectedOutputName,
            &m_selectedOutputPlugin,
            &error);

        if (ok) {
            ok = mpd_run_play(connection);

            if (!ok) {
                error = mpdError(connection);
            }
        }
    } else {
        // PAUSE mode: explicitly enforce pause.
        ok = mpd_run_pause(connection, true);

        if (!ok) {
            error = mpdError(connection);
        }
    }

    mpd_connection_free(connection);

    if (!ok) {
        emit statusChanged(error);
        return;
    }

    // Change the UI mode only after the MPD command succeeds.
    m_pauseMode = !m_pauseMode;
    emit playbackModeChanged(m_pauseMode);
}

QList<MpdAudioOutput> MpdControls::availableOutputs(QString* error)
{
    if (error) {
        error->clear();
    }

    QString localError;
    auto* connection = openMpdConnection(&localError);

    if (!connection) {
        if (error) {
            *error = localError;
        }
        return {};
    }

    const auto outputs = readMpdOutputs(connection, &localError);
    mpd_connection_free(connection);

    if (!localError.isEmpty()) {
        if (error) {
            *error = localError;
        }
        return {};
    }

    // Restore the previous choice, or choose and persist the first output.
    // The reordered list means non-ALSA outputs come first, then ALSA,
    // with HTTPD outputs at the very end.
    restoreOrChooseOutput(
        outputs, &m_selectedOutputId,
        &m_selectedOutputName, &m_selectedOutputPlugin);

    return outputs;
}

void MpdControls::selectOutput(const MpdAudioOutput& output)
{
    // Remember the selection only. The MPD output configuration is changed
    // when playback is started.
    m_selectedOutputId = static_cast<int>(output.id);
    m_selectedOutputName = output.name;
    m_selectedOutputPlugin = output.plugin;

    QSettings settings;
    settings.beginGroup(QStringLiteral("MpdControls"));
    settings.setValue(QStringLiteral("selectedOutputId"), m_selectedOutputId);
    settings.setValue(QStringLiteral("selectedOutputName"), m_selectedOutputName);
    settings.setValue(QStringLiteral("selectedOutputPlugin"), m_selectedOutputPlugin);
    settings.endGroup();
}

bool MpdControls::tracksForCurrentPlaylist(Fooyin::TrackList* tracks,
                                           QString* error) const
{
    if (tracks) {
        tracks->clear();
    }
    if (error) {
        error->clear();
    }

    if (!tracks || !m_playlistSelection || !m_trackSelection) {
        if (error) {
            *error = QStringLiteral("Fooyin playlist selection is unavailable");
        }
        return false;
    }

    Fooyin::Playlist* playlist = m_playlistSelection->currentPlaylist();
    if (!playlist) {
        if (error) {
            *error = QStringLiteral("No playlist is selected in Fooyin");
        }
        return false;
    }

    const Fooyin::UId playlistId = m_playlistSelection->currentPlaylistId();
    const auto target = m_trackSelection->playlistSelectionTarget(playlistId);

    if (target
        && target->selection.playlistId
        && *target->selection.playlistId == playlistId
        && !target->selection.tracks.empty()) {
        // A selection in the currently selected playlist takes precedence.
        *tracks = target->selection.tracks;
    } else {
        // No selected rows: use every track in the UI-selected playlist.
        *tracks = playlist->tracks();
    }

    if (tracks->empty()) {
        if (error) {
            *error = QStringLiteral("The selected Fooyin playlist is empty");
        }
        return false;
    }

    return true;
}

void MpdControls::addTracks(const Fooyin::TrackList& tracks,
                            bool toStoredPlaylist,
                            bool clearFirst)
{
    if (m_busy) {
        emit statusChanged(QStringLiteral("An MPD operation is already running"));
        return;
    }

    if (tracks.empty()) {
        emit statusChanged(QStringLiteral("There are no tracks to add"));
        return;
    }

    QString error;
    const QStringList uris = makeMpdUris(tracks, m_library, &error);
    if (!error.isEmpty()) {
        emit statusChanged(error);
        return;
    }

    const MpdConfig config = readMpdConfig();

    m_busy = true;
    emit busyChanged(true);

    auto* watcher = new QFutureWatcher<QString>(this);

    connect(watcher, &QFutureWatcher<QString>::finished, this,
            [this, watcher]() {
                m_busy = false;
                emit busyChanged(false);
                emit statusChanged(watcher->result());
                watcher->deleteLater();
            });

    watcher->setFuture(QtConcurrent::run(
        [uris, toStoredPlaylist, clearFirst, config]() {
            return runMpdOperation(uris, toStoredPlaylist, clearFirst, config);
        }));
}


MpdControlsWidget::MpdControlsWidget(MpdControls* controls, QWidget* parent)
    : FyWidget{parent}
    , m_controls{controls}
    , m_queueButton{new QPushButton(tr("MPD queue add"), this)}
    , m_playlistButton{new QPushButton(tr("MPD playlist add"), this)}
    , m_clearCheckBox{new QCheckBox(tr("MPD clear"), this)}
    , m_previousButton{new QToolButton(this)}
    , m_playPauseButton{new QToolButton(this)}
    , m_nextButton{new QToolButton(this)}
{
    auto* layout = new QHBoxLayout(this);

    layout->setContentsMargins(0, 0, 0, 0);

    // Existing export controls.
    layout->addWidget(m_queueButton);
    layout->addWidget(m_playlistButton);
    layout->addWidget(m_clearCheckBox);

    m_clearCheckBox->setChecked(false);

    // Separate the export controls from playback controls.
    layout->addSpacing(8);

    // Playback buttons.
    m_previousButton->setIcon(
        style()->standardIcon(QStyle::SP_MediaSkipBackward));

    m_playPauseButton->setIcon(
        style()->standardIcon(QStyle::SP_MediaPlay));

    m_nextButton->setIcon(
        style()->standardIcon(QStyle::SP_MediaSkipForward));

    for (auto* button : {
             m_previousButton,
             m_playPauseButton,
             m_nextButton
         }) {
        button->setAutoRaise(true);
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setIconSize(QSize(18, 18));
        button->setFixedSize(28, 28);

        layout->addWidget(button);
    }

    // Play/Pause right-click behaviour.
    m_playPauseButton->setToolTip(
        tr("Right-click to select MPD audio output"));

    m_playPauseButton->setContextMenuPolicy(
        Qt::CustomContextMenu);

    // Existing export connections.
    connect(m_queueButton, &QPushButton::clicked, this,
            [this]() { add(false); });

    connect(m_playlistButton, &QPushButton::clicked, this,
            [this]() { add(true); });

    connect(m_controls, &MpdControls::busyChanged, this,
            [this](bool busy) {
                m_queueButton->setDisabled(busy);
                m_playlistButton->setDisabled(busy);
            });

    connect(m_controls, &MpdControls::statusChanged,
            this, &MpdControlsWidget::showResult);

    // New playback connections.
    connect(m_previousButton, &QToolButton::clicked,
            m_controls, &MpdControls::previous);

    connect(m_playPauseButton, &QToolButton::clicked,
            m_controls, &MpdControls::togglePlayback);

    connect(m_nextButton, &QToolButton::clicked,
            m_controls, &MpdControls::next);

    connect(m_controls, &MpdControls::playbackModeChanged,
            this, [this](bool pauseMode) {
                m_playPauseButton->setIcon(
                    style()->standardIcon(
                        pauseMode
                            ? QStyle::SP_MediaPause
                            : QStyle::SP_MediaPlay));
            });

    connect(m_playPauseButton,
            &QToolButton::customContextMenuRequested,
            this,
            &MpdControlsWidget::showOutputMenu);
}


void MpdControlsWidget::showOutputMenu(const QPoint& pos)
{
    QMenu menu(this);

    // This command is deliberately independent of output discovery, so it
    // remains available even if MPD cannot return its output list.
    QAction* stopAction = menu.addAction(tr("Stop playback"));
    connect(stopAction, &QAction::triggered,
            m_controls, &MpdControls::stopPlayback);

    menu.addSeparator();

    QString error;
    const auto outputs = m_controls->availableOutputs(&error);

    if (!error.isEmpty()) {
        QAction* unavailableAction = menu.addAction(
            tr("Audio outputs unavailable"));
        unavailableAction->setEnabled(false);
    } else if (outputs.isEmpty()) {
        QAction* emptyAction = menu.addAction(
            tr("No MPD audio outputs available"));
        emptyAction->setEnabled(false);
    } else {
        QActionGroup group(&menu);
        group.setExclusive(true);

        for (const auto& output : outputs) {
            QAction* action = menu.addAction(output.name);
            action->setCheckable(true);
            action->setData(output.id);
            group.addAction(action);
            action->setChecked(
                static_cast<int>(output.id) == m_controls->selectedOutputId());

            connect(action, &QAction::triggered,
                    this, [this, output]() {
                        m_controls->selectOutput(output);
                    });
        }
    }

    menu.exec(m_playPauseButton->mapToGlobal(pos));

    if (!error.isEmpty()) {
        showResult(error);
    }
}

void MpdControlsWidget::add(bool toStoredPlaylist)
{
    Fooyin::TrackList tracks;
    QString error;

    if (!m_controls->tracksForCurrentPlaylist(&tracks, &error)) {
        showResult(error);
        return;
    }

    if (tracks.size() > 50) {
        const QString destination = toStoredPlaylist
            ? tr("the MPD playlist “Filtered playlist”")
            : tr("the MPD queue");

        const auto answer = QMessageBox::question(
            this,
            tr("Confirm MPD addition"),
            tr("Add %1 tracks to %2?")
                .arg(static_cast<qulonglong>(tracks.size()))
                .arg(destination),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel);

        if (answer != QMessageBox::Yes) {
            return;
        }
    }

    const bool clearFirst = m_clearCheckBox->isChecked();

    // Consume the one-shot clear option only when proceeding with the add.
    m_clearCheckBox->setChecked(false);

    m_controls->addTracks(tracks, toStoredPlaylist, clearFirst);
}

void MpdControlsWidget::showResult(const QString& message)
{
    QMessageBox dialog(QMessageBox::Information,
                       tr("MPD"),
                       message,
                       QMessageBox::Ok,
                       this);
    dialog.setDefaultButton(QMessageBox::Ok);
    dialog.setEscapeButton(QMessageBox::Ok);
    dialog.exec();
}

QString MpdControlsWidget::name() const
{
    return tr("MPD controls");
}

QString MpdControlsWidget::layoutName() const
{
    return QStringLiteral("MPDControls");
}
