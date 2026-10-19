# Updating Summit from the browser (issue #72)

Summit finds new builds of itself on GitHub and installs them, so nobody has
to watch the README's build tables and run `pkgman` by hand.

## What the user sees

- **File › Check for Updates…** (under About Summit) asks GitHub at once and
  always answers: the update window when there is a newer build, otherwise
  "Summit is up to date" with the build, "This build of Summit is newer than
  the latest release", or why it could not check.
- **Preferences › General › Updates**: "Check for a new build of Summit once a
  day" (on by default), a **Check Now** button and when GitHub last answered.
  With the box ticked Summit looks half a minute after it starts and then
  every hour whether a day has passed since the last check; if so it checks
  quietly. Only a newer build shows anything.
- **The update window** names the new build ("Summit
  0.1.0~git20261019.0340-1 (15fce3d), built 19 October 2026, for air/OS
  (x86_64)") and yours, and lists what changed: the commits between the two
  builds, newest first, with their dates. Updates of the README and the
  documentation (CI's `[skip ci]` commits) are counted, not listed. **Show on
  GitHub** opens the comparison in a tab; **Later** closes the window (the
  daily check shows it again tomorrow).
- **Install and Restart** downloads both packages with a progress bar
  ("Downloading summit_webkit (54.0 of 84.1 MiB)…"; **Cancel** stops it),
  checks them, installs them, and restarts Summit with its windows and tabs,
  also when Preferences say not to reopen them. Private windows are not
  saved, as at any quit. A page that keeps its window open when asked to
  close (unsaved work) stops the restart; the window then says the new build
  starts the next time Summit starts. When something fails, a message gives
  the reason (pkgman's own output if it was pkgman), Summit is as it was, and
  the button becomes **Try Again**.
- **About Summit** shows the packages that run: "Summit
  0.1.0~git20261019.0340-1 (15fce3d)" and "Summit WebKit
  1.10.1~git20261019.0340-1", or "Not installed from a package".
- A copy that does not run from installed packages (a bundle, a build of
  one's own) never checks by itself. File › Check for Updates… says that it
  cannot update itself, names the latest build for its system and offers to
  open the release page.

## How it works

`src/core/UpdateCheck.{h,cpp}` has everything that needs neither the network
nor Haiku, tested on the host in `summit_update_tests`;
`src/ui/UpdateChecker.{h,cpp}` asks GitHub, shows the windows and installs.

- **What runs.** packagefs gives each file of an installed package the
  attributes `SYS:PACKAGE` ("summit-0.1.0~git20261019.0340-1") and
  `SYS:PACKAGE_FILE`. Summit reads them from its executable and from the
  `libWebKit.so.1` it loaded, so it knows both packages without the package
  kit (which it does not link). The package file must be in the system's or
  the home folder's `packages` (the second means `pkgman install -H`), and
  both packages in the same one. The commit comes from the package's
  description, which CI writes as "…made by air/OS CI from Summit 15fce3d…",
  read with `package list -i`.
- **Which release.** The air/OS build (`SUMMIT_AIROS`, `core/BuildTarget.h`)
  updates from the release `latest`, the stock Haiku build from
  `latest-haiku`, through
  `https://api.github.com/repos/jmgasper/summit/releases/tags/<tag>`; the
  architecture is the one Summit was compiled for. GitHub names release files
  with '.' where package versions have '~'; the table of SHA-256 sums in the
  release's notes gives the packages' real names (without it, the '~' goes
  before the first part after the major version that starts with a letter).
  Versions compare as the package kit compares them (`BPackageVersion`:
  natural order, a pre-release before its release, then the revision). The
  release must have both packages for the architecture, from the same build
  (the same `git` date and time and revision), at `https://` addresses under
  the repository's release downloads; a release that does not is refused
  before anything is downloaded. A newer browser is an update; so is the same
  browser with a newer engine, which installing the pair repairs.
- **What changed** comes from the compare API
  (`/compare/<your commit>...<release commit>?per_page=100`). If that fails
  (rate limit, no commit in an old package's description) the window says so
  and still offers the update.
- **Installing.** The files go to `/boot/system/cache/tmp/summit-update-*`,
  which is removed afterwards whatever happened. Summit first makes sure the
  disk has room for them twice (pkgman copies them). Each download must have
  the size the release gives and the SHA-256 it states: GitHub's own
  `digest` for the file, the release notes' table and, if the release has
  one, a `SHA256SUMS` file; these must agree with each other. Then `package
  list -i` must name the right package, version and architecture, and the
  system its description names ("The air/OS build", "The Haiku R1/beta6
  build") must be this one. Both are installed in one transaction, `pkgman
  install -y [-H] summit_webkit-….hpkg summit-….hpkg`, so one is never
  installed without the other; afterwards the executable's `SYS:PACKAGE`
  must name the new version. Commands run through `/bin/sh` without the
  test copy's `LIBRARY_PATH`, and report their exit status in their output
  (the engine's process reaper may collect the shell first).
- **Restarting** (`kUpdateRestart`) is the quit of File › Quit, which saves
  the session and lets pages ask about unsaved work (#58), followed by the
  start of the same executable (now the new package's file) with `--profile
  <this profile> --restore-session`. The new instance waits for the old one
  to finish writing the profile (`WaitForExitingInstance`).
  Restarting used to hand the new Summit all of the old one's descriptors
  (`load_image` passes on whatever is not close-on-exec): with the MCP
  server on, the old listening socket kept the port, so the new server could
  not listen and requests hung, and stderr was the old log capture's pipe,
  which nobody read. `StartAgain` now marks every descriptor close-on-exec
  and points stderr back where it went before the capture
  (`mcplog::RestoreStderr`). This also fixes starting Summit while it quits.
- **Rate limits and failures.** GitHub allows 60 requests an hour from one
  address without an account; a check is one request, two when there is an
  update. A 403 or 429 with no requests left (or `Retry-After`) becomes "GitHub
  allows only so many requests an hour from one network… Try again in 25
  minutes." A check counts for the day when GitHub answered, whatever it
  answered; without a network the daily check tries again an hour later.
  Automatic checks never show errors (they go to stderr).

## Testing

Three environment variables make a test copy behave like an installed one
without touching the system:

- `SUMMIT_UPDATE_INSTALLED="summit-0.1.0~git20261017.1829-1
  summit_webkit-1.10.1~git20261017.1829-1 56fb359"` pretends those packages
  (and commit) are installed. A pretended build always runs as a test.
- `SUMMIT_UPDATE_DRY_RUN=1` replaces the pkgman transaction: both packages
  are downloaded and checked as for a real update, then unpacked with
  `package extract` into a scratch folder, which must hold `apps/Summit` and
  the engine; the pkgman command that would have run goes to stderr. Summit
  then restarts as after a real update.
- `SUMMIT_UPDATE_API` replaces `https://api.github.com/repos/jmgasper/summit`
  (plain HTTP allowed, download addresses not checked), for a stand-in server.

The MCP server's `updates` tool reports what runs, the last check and the
update window, and with `do` checks (`check`) or presses the window's buttons
(`install`, `later`, `changes`).

On the X399 (air/OS, x86_64) with a test copy on CI's 0340 engine
(`claude-s35/bundle-up*`), 19 October 2026:

- A copy pretending the 17 October build (56fb359) was installed found
  0.1.0~git20261019.0340-1 (15fce3d) by the daily check 30 s after starting,
  listed its five browser commits (16 README/docs commits counted), opened
  the comparison with Show on GitHub, and Install and Restart downloaded both
  packages (88 MiB), checked sizes and SHA-256, read both with `package list
  -i`, unpacked them into the scratch folder and restarted the copy with its
  three tabs while Preferences said not to reopen windows; the temporary
  folder was gone afterwards.
- File › Check for Updates… (the window's message), Preferences' Check Now,
  the checkbox (saved in the profile), About Summit, "up to date" (pretending
  0340), "newer" (pretending a 20 October build) and the plain copy's "not
  installed from a package" with Open Release Page.
- Against a stand-in server: a rate limit ("Try again in 25 minutes"), HTTP
  503, no network (not counted as the day's check), a release whose engine
  is from another build (refused), a refused comparison (the window still
  opens), checksums that disagree (refused before downloading), a download
  whose SHA-256 is wrong (refused after downloading), a `SHA256SUMS` file
  (checked, then unpacked and restarted) and Cancel during the download.
- pkgman itself was not run on the X399, whose Summit runs from test bundles.
