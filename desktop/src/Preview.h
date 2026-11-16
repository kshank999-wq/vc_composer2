// Preview builds (packaging: VCC_PREVIEW_BUILD): free, time-limited builds
// for testing before release. They say "Preview" in the title and About box
// and run until VCC_PREVIEW_EXPIRES (set by the build, 60 days out). After
// that, as a lapsed licence does, the app stays useful but stops saving:
// projects still open, play, print and export.
#pragma once

#include <juce_core/juce_core.h>

namespace vcc::desktop::preview {

bool is_preview() noexcept;
/// "2027-01-15"; empty in a release build.
juce::String expires();
/// Whole days left (0 on the last day, negative once ended), counted from
/// `today` ("YYYY-MM-DD", default: now, UTC).
int days_left(const juce::String& today = {});
bool ended(const juce::String& today = {});
/// "Preview - 45 days left", "Preview ended 2027-01-15"; empty in a release.
juce::String status(const juce::String& today = {});

}  // namespace vcc::desktop::preview
