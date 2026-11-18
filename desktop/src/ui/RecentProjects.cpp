#include "ui/RecentProjects.h"

#include "ui/hw/HwTheme.h"

namespace vcc::desktop::recent {

juce::StringArray projects()
{
    juce::StringArray list;
    list.addLines(hw::ui_settings().getValue("recent_projects", ""));
    list.removeEmptyStrings();
    return list;
}

void remember(const juce::File& project_file)
{
    if (project_file == juce::File()) return;
    auto list = projects();
    list.removeString(project_file.getFullPathName());
    list.insert(0, project_file.getFullPathName());
    while (list.size() > 12) list.remove(list.size() - 1);
    auto& st = hw::ui_settings();
    st.setValue("recent_projects", list.joinIntoString("\n"));
    st.setValue("last_project_folder", project_file.getParentDirectory().getFullPathName());
    st.saveIfNeeded();
}

void clear()
{
    hw::ui_settings().setValue("recent_projects", "");
    hw::ui_settings().saveIfNeeded();
}

juce::File last_folder() { return juce::File(hw::ui_settings().getValue("last_project_folder", "")); }

}  // namespace vcc::desktop::recent
