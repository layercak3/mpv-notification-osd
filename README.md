# mpv-notification-osd

This is a C plugin for mpv which provides a small OSD in a notification for
feedback while mpv is unfocused and the playing track changes or playback is
remotely controlled from JSON IPC/MPRIS. It supports embedded and external cover
art, as well as a preview thumbnail of the current time position of a video.

## Requirements

* mpv built with C plugins support
* mpv client API header, make, pkgconf, C compiler
* an XDG desktop notifications server
* GLib
* GdkPixbuf
* libnotify
* libswscale
* GNU/Linux (pipe2, timerfd)

## Usage

Run `make` to build the plugin, then copy the resulting `notification-osd.so`
file to your scripts directory. Alternatively, you can run `make install` as
root to install to the system/DESTDIR or as non-root to install to
`~/.config/mpv/scripts`.

There are a few options which can be configured using the script-opts facility.
They are documented in [Script options](#script-options). Make sure to use the
client name (i.e. `notification_osd.conf` instead of `notification-osd.conf`).

## Cover art and thumbnails

The `screenshot-raw` command is run when a video track is loaded or video
parameters change. For videos, this is also repeatedly run whenever
`percent-pos` changes and the notification is open. So, the
image/video/lavfi-complex that is selected will be shown in the notification
with whatever filters or equalizer parameters it has (if any).

The screenshot is downscaled using libswscale preserving aspect ratio so that
the notification server can handle it quickly and without using a lot of CPU
time. By default, the maximum dimensions are 64x64 and the bicubic option is
used.

## Notification lifetime

Notifications are only shown while the player is not considered to be "focused".
Currently, "focused" is defined as the `focused` property being true or the
mouse being hovered over the window. The intent is that notifications don't need
to be shown while the user is interacting with the player window, only when they
are doing something else. There is also support to manually or externally
dictate if the notification is shown using script messages.

After a triggering property change or event occurs which opens the
notification, it will expire after 10 seconds by default. During this time, the
text in the notification and thumbnail preview will be updated as player
properties change. Because setting an expire time for the server would result in
the timer being reset on each update, an expire time isn't set and the
notification server should be configured to not set one for the "mpv" category.
Instead, the plugin closes the notification itself using its own timer.

This means that dismissing it through the notification server can be futile if
player properties change and the notification is immediately sent again soon
after dismissal. The plugin registers a script message `close` (see the [Script
messages](#script-messages) section) which can be used to tell the plugin that
the notification should be closed. You can configure your notification server to
send this script message instead of trying to dismiss it. For example, using
mako:

```
[app-name=ampv]
on-button-right=exec ~/.local/bin/mpvctl ampv ic 'script-message-to notification_osd close'
on-touch=exec ~/.local/bin/mpvctl ampv ic 'script-message-to notification_osd close'
```

## Script messages

The following script messages will be acted upon:

* `close`: Disarm the timer and close the notification.
* `open`: Force the notification to stay open until the `close` message is sent.
* `reload-config`: Reload the configuration file (and apply runtime options over
  it).

## Script options

Some script options can be set using the script-opts facility. These can be
changed at runtime.

* `expire_timeout` (integer): Seconds to wait before closing the notification
  after opening it.
* `ntf_app_icon` (string): Symbolic icon name or filename. If this is an empty
  string, the app icon won't be sent. (default: mpv)
* `ntf_category` (string): Notification category. If this is an empty string,
  the category won't be sent. (default: mpv)
* `ntf_urgency` (choice): Notification urgency from "low", "normal", or
  "critical". If this is an empty string or an incorrect choice, "low" will be
  used. (default: low)
* `send_thumbnail` (boolean): Generate and send thumbnails. (default: yes)
* `send_progress` (boolean): Set the `value` hint to the value of `percent-pos`
  to add a progress bar/background. (default: yes)
* `send_sub_text` (boolean): Send the current subtitle or lyric text in the body
  (`sub-text` property). (default: yes)
* `thumbnail_size` (integer): The maximum width or height that the scaled
  thumbnail will have. The other dimension may be decreased to preserve aspect
  ratio. You probably want to set this to the size that your notification server
  displays thumbnails such that it doesn't perform any scaling itself. (default:
  64)
* `screenshot_flags` (string): Screenshot flags to submit to the
  `screenshot-raw` command. You probably want to leave this as "video" for video
  without subtitles or "subtitles" to include subtitles. See the mpv manual for
  details. (default: video).
* `thumbnail_scaling` (choice): Thumbnail scaling option from "fast-bilinear",
  "bilinear", "bicubic", or "lanczos". See `enum SwsFlags` in swscale.h for
  details. If this is an empty string or an incorrect choice, "bicubic" will be
  used. (default: bicubic)
* `disable_scaling` (boolean): Don't scale the thumbnail, and instead send the
  screenshot directly to the notification server. This can be slow depending on
  the notification server, and its scaling method will likely be lower quality.
  (default: no)
* `focus_manual` (boolean): Always consider the player to be focused,
  effectively never showing the notification unless the script messages are used
  to manually/externally show it. (default: no)
* `perfdata` (boolean): Collects and prints some frame timing information mostly
  related to screenshots. (default: no)

## License

GPL-3.0-or-later, see COPYING.
