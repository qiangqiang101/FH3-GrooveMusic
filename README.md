# Groove Music Radio for Forza Horizon 3

Brings back the radio's **Groove** station in Forza Horizon 3 on Azimuth, with your own music. FH3's
Groove (Xbox Music) service was shut down; this plugin answers the game's calls itself, so the game's
own Groove player plays:

- **your music folder**: every folder in it is a playlist (MP3, FLAC and WAV); the game's Shuffle
  option shuffles them;
- **Spotify** (Premium), through Spotify's Soloist;
- **YouTube Music, NetEase Cloud Music and QQ Music**: choose the app as the playlist and play
  something in it; the station plays the app's sound.

Pick the playlist in **Settings > Audio**. On the Audio page, **X** opens your music folder.

With [FH3 ANNA](https://github.com/qiangqiang101/FH3-ANNA) installed, ANNA's menu (D-pad Down while
driving) gets **Groove Music** while the radio plays the Groove station: pick a playlist, or skip to
the previous or next song.

## Install

Needs [Extra Options](https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions) for FH3. Next to
`azimuth.exe`, put `GrooveMusicRadio.dll` in `mods\Forza Horizon 3\GrooveMusicRadio\plugins\` and
`GrooveMusicRadio.ini` in `mods\Forza Horizon 3\GrooveMusicRadio\`. The INI explains every setting:
the music folder, shuffle, tags, Spotify's Soloist (put `soloist.exe` and your Soloist API key there)
and the music apps. Keep your own INI when you update. Log lines are in `FH3_ExtraOptions.log`, marked
`PLUGIN: Groove Music Radio`.

## Building

`sh build.sh [out.dll]` with [llvm-mingw](https://github.com/mstorsjo/llvm-mingw). `sdk/` holds
copies of Extra Options' `fh3_plugin.h` and FH3 ANNA's `fh3anna.h`.

## Credits

The music library, Spotify and music-app sources come from my XboxMusicRadio for Forza Horizon 2.
Music files are decoded with
[dr_libs](https://github.com/mackron/dr_libs) (dr_mp3, dr_flac, dr_wav) by David Reid, public domain or
MIT No Attribution (`third_party/dr_libs`).
