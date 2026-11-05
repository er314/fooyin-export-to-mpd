# Export tracks to MPD - Fooyin plugin

## Summary

This is a plugin for [fooyin](https://github.com/fooyin/fooyin).
<br>It allows to export the Fooyin current track list, or current selected tracks if any, to MPD.
<br>Use case: You love MPD ecosystem ? But you miss the capability to natively manage your own user-created tags, embedded in the audio files ?
-> This plugin is for you.
<br>It allows to use Fooyin powerful handling of any tag (advanced search engine, tag editing), then, once you have filtered and/or selected the tracks you want to play,
- Button "MPD queue add" performs addition of these tracks to the MPD queue
- Button "MPD playlist add" performs addition of these tracks to MPD playlist "Filtered playlist"
- Checkbox "MPD clear" performs cleanup of the queue or playlist before addition

![Screenshot](README-screenshot.jpg)

## Pre-requisites

- Fooyin
- libmpdclient2

## Installation

- Build from source (see Fooyin build doc)
- Within fooyin, install & enable plugin "Export to MPD" (file 'fyplugin_mpdadd.so').

## Usage

Use Fooyin normally, browsing & searching for tracks, in order to populate the active track list.
Moreover, you can optionally select a subset of the active tracks.
Once satisfied with the track list (full track list, or selected subset), now you can perform export of these tracks to MPD:
- (a) If your intent is to start over from scratch (replacing any existing contents), check the "MPD clear" checkbox ; otherwise leave it unchecked
- (b) If your intent is to populate the play queue, click on Button "MPD queue add" -> This performs addition of the tracks to the MPD play queue
- (c) If your intent is to populate a playlist, click on button "MPD playlist add" -> This performs addition of the tracks to MPD playlist "Filtered playlist"

You can further iterate browsing + (b) or (c) for adding more tracks into MPD (queue or playlist), until all expected tracks are added. 

Then, use your regular MPD client (on smarpthone or computer):
- The queue (or playlist) is there, as just populated.
- Use the MPD client normally, e.g. to start playing the tracks.

## Notice

This (modest) software was (modestly) coded with the help of AI.

## Legal

This software is released under [GPLv3](https://www.gnu.org/licenses/gpl-3.0.en.html).
