# Browser profiles (#157)

Profiles as in Safari 17: each has its own history, bookmarks, settings,
extensions, cookies and website data, and its own windows.

## What the user sees

- **File › New Window in Profile ▸** lists the profiles (the window's own is
  marked) and **Manage Profiles…**. Choosing one opens a window of it.
- **Preferences › Profiles** lists them with their colours and which are open:
  **New Profile…** and **Edit…** (name and colour), **Open Window**, and
  **Delete…**, which asks first and moves the profile's folder to the Trash. A
  profile whose windows are open, or the default one, cannot be deleted.
- While there is more than one profile, every window shows its profile: the
  title ends in "— Summit (Work)", and the toolbar has a badge with the name on
  the profile's colour. With one profile nothing changes.
- **File › Quit** (in any profile's window) quits every profile. Their windows
  open again with the default profile's the next time Summit starts (when
  Preferences › General restores windows). Closing a profile's last window
  ends only that profile.

The profile Summit always had is the default one, named "Personal"; nothing is
migrated.

## How it works

- A profile is a profile folder, what `--profile` names. The default profile's
  folder (`~/config/settings/SummitModern`, or the `--profile` folder Summit
  was started with) keeps `profiles.json`, the list with names, colours and a
  `reopen` flag, and the other profiles in `Profiles/<id>/`
  (`src/core/BrowserProfiles.*`, tested in `tests/CoreTests.cpp`). A Summit
  started with `--profile …/Profiles/<id>` knows it is that profile
  (`BrowserProfiles::Locate`).
- **One Summit process per profile.** Summit's state is per process
  throughout (`main.cpp`: the profile, the engine context, extensions, site
  permissions, downloads, containers, the debugging server), so a profile gets
  all of it by running in a process of its own, and its extensions, cookies
  and website data (its own `BWebKitContext` on its folder) come for free.
- To run one executable several times Summit is now `B_MULTIPLE_LAUNCH`
  (`resources/Summit.rdef`); with `B_SINGLE_LAUNCH` the registrar hands every
  second start of the same file to the first, whatever `--profile` says. One
  Summit per profile folder is kept by the folder's lock (#93): a second start
  of a profile that runs hands its pages, web apps, `--new-window` and
  notification clicks to the running one (`LockProfile`,
  `HandToRunningCopy`) and ends. That hand-over takes about 0.1 s on the X399
  (a new process that loads the libraries), where the registrar's took none.
- Processes find each other by the team the profile's `lock` file names, when
  the lock is held (`RunningProfileTeam`, `src/ui/Profiles.cpp`). Opening a
  window of a running profile sends it `--new-window`; otherwise Summit starts
  itself with `--profile` (posix_spawn, with only stdin/stdout/stderr open,
  stderr where it went before the debugging server's log capture).
  Preferences changes are sent to every running profile (`kProfilesChanged`),
  and File › Quit asks the others to quit (`kQuitWithOtherProfiles`).
- Summit's own notifications carry `--profile` so that a click reaches the
  right profile. The "Summit exits" port that a new start waits on is per
  profile folder. Only the default profile does the daily update check.

## Testing

`SUMMIT_MCP_PORT` in the environment (as test runs set it) moves to the base
port plus the profile's place in the list for profiles Summit starts itself,
so each has its debugging server (7901 default, 7902 the first other profile).

Verified on the X399 (1 Nov 2026): a second profile made in Preferences,
windows of it opened from the other profile and back, a cookie set in each
not visible in the other, a bookmark in one only, rename and colour change
shown in both processes' windows, File › Quit from either quitting both, the
next start reopening both with their windows, cookies and bookmarks, closing
the last window of one leaving the other running, and Delete refused while
open and moving the folder to the Trash once closed.

## Not done

- One process for several profiles (the issue's ideal): every profile costs a
  UI process, a network process and its own extension background pages.
- Links from other applications open in the default profile.
- Extension notifications (built by the engine) carry no profile; a click on
  one from another profile goes to the default profile, which ignores it.
- An update restarts the profile whose window installed it (normally the
  default one, as only it checks daily); the others quit with it and come
  back with the default profile's next start.
- Profile icons (Safari has symbols as well as colours).
