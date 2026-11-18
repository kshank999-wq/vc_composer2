// Where projects live: the ones opened or saved lately, newest first, on
// whatever drive or cloud folder they are (File > Open Recent), and the
// folder the next Open, Save As or New Project starts in.
#pragma once

#include <juce_core/juce_core.h>

namespace vcc::desktop::recent {

juce::StringArray projects();
/// Records a project just opened, saved or created.
void remember(const juce::File& project_file);
void clear();
/// The folder the last project was saved or opened in (may not exist any more).
juce::File last_folder();

}  // namespace vcc::desktop::recent
