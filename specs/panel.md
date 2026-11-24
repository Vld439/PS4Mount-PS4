# Library panel

### PANEL-1: selection survives library refreshes

The web panel and the PS4 app present the library as a shelf with details for the selected game.
When the game remains visible after a refresh, its selection and the focused cover remain.
Filtering can select the first matching game. Navigating or selecting a cover makes no write
request. Adding, removing and restoring games continue to use their existing actions.
Check: test for selection and requests (`test/web/panel.test.js`), read for layout and focus geometry.
Source: approved Estantería HTML prototype, with PS4-inspired blues.

### PANEL-2: the console app can navigate and go back

The app supports directional focus and confirm on available controls. Back closes the current
dialog or page before it exits the panel, including when the app uses the ES5 fallback.
Check: test for the keyboard route and fallback (`test/web/panel.test.js`), read for gamepad input.
Source: user request for TV and controller operation.
