# Luban OS app: layout baselines

Where each named control of a key page lies (`state` anchors, window
pixels), at scales 1, 1.5 and 2 and in English and Chinese, as reviewed
(design 2026-10-16 L §13.8). `LubanOs.KeyPagesHoldTheirLayoutAtEveryScaleInBothLanguages`
holds the app to them, within the larger of 8 pixels and 3 % of the
window, when it draws with the fonts Luban OS ships (`fonts.txt` names the
set; `tools/ci/luban_fonts.sh` fetches it into `LUBAN_OS_FONTS`, as the
Linux CI lanes do). With a machine's own fonts a control's place depends on
those fonts: the test then holds each control in the window, apart from the
others and large enough to hit, and to no baseline.

A change of layout updates them in the same PR: the test records any
missing baseline into `<artifacts>/luban-os/baseline/`, and the contact
sheet (`<artifacts>/luban-os/contact-sheet.ppm`) shows every page for the
review.
