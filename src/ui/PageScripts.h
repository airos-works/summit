#pragma once

namespace summit {
// Scripts every web tab gets at document start, in every frame, in the
// page's own world (BWebKitView::AddUserScript). Each talks to the browser
// through its script message handler, with JSON text.
//   summitPopups:    {url, target} for each window.open() the browser refused.
//   summitPip:       the Picture in Picture API: {type: "enter", token},
//                    {type: "exit"}, {type: "context", token} (the video under
//                    a context menu). window.__summitPipLargest() names the
//                    largest video of the frame. A Picture in Picture button
//                    shows over the video under the pointer.
//   summitPasswords: {type: "submit", username, password} after a sign-in,
//                    {type: "query"} when the user starts on a sign-in form
//                    (the reply lists the site's saved accounts as JSON).
extern const char* const kPopupScript;
extern const char* const kPictureInPictureScript;
extern const char* const kPasswordScript;
// Passkey autofill (#111): tells the browser where a sign-in field marked
// autocomplete="… webauthn" is when the user goes into it, so that the
// native menu of the site's passkeys opens under it. Runs in every frame; a
// frame's field is reported in the page's coordinates.
extern const char* const kPasskeyFieldScript;
}
