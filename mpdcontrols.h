#pragma once

#include <core/track.h>
#include <gui/fywidget.h>

#include <QObject>
#include <QString>

class QCheckBox;
class QPushButton;

namespace Fooyin {
class CurrentPlaylistController;
class MusicLibrary;
class TrackSelectionController;
}

class MpdControls final : public QObject
{
    Q_OBJECT

public:
    explicit MpdControls(QObject* parent = nullptr);

    void setFooyinServices(Fooyin::MusicLibrary* library);
    void setGuiServices(Fooyin::CurrentPlaylistController* playlistSelection,
                        Fooyin::TrackSelectionController* trackSelection);

    bool tracksForCurrentPlaylist(Fooyin::TrackList* tracks,
                                  QString* error) const;
    void addTracks(const Fooyin::TrackList& tracks,
                   bool toStoredPlaylist,
                   bool clearFirst);

signals:
    void busyChanged(bool busy);
    void statusChanged(const QString& message);

private:
    Fooyin::MusicLibrary* m_library{nullptr};
    Fooyin::CurrentPlaylistController* m_playlistSelection{nullptr};
    Fooyin::TrackSelectionController* m_trackSelection{nullptr};
    bool m_busy{false};
};

class MpdControlsWidget final : public Fooyin::FyWidget
{
    Q_OBJECT

public:
    explicit MpdControlsWidget(MpdControls* controls,
                               QWidget* parent = nullptr);

    [[nodiscard]] QString name() const override;
    [[nodiscard]] QString layoutName() const override;

private:
    void add(bool toStoredPlaylist);
    void showResult(const QString& message);

    MpdControls* m_controls;
    QPushButton* m_queueButton;
    QPushButton* m_playlistButton;
    QCheckBox* m_clearCheckBox;
};
