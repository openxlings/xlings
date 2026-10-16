# Luban OS app: layout baselines

Where each named control of a key page lies (`state` anchors, window
pixels), at scales 1, 1.5 and 2 and in English and Chinese, as reviewed
(design 2026-10-16 L §13.8). `LubanOs.KeyPagesHoldTheirLayoutAtEveryScaleInBothLanguages`
holds the app to them within what another machine's fonts move a control
(the larger of 8 pixels and 3 % of the window).

A change of layout updates them in the same PR: the test records any
missing baseline into `<artifacts>/luban-os/baseline/`, and the contact
sheet (`<artifacts>/luban-os/contact-sheet.ppm`) shows every page for the
review.
