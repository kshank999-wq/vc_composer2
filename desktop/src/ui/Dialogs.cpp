#include "ui/Dialogs.h"

namespace vcc::desktop {

// Placeholders: the three tool windows are being built in a follow-up commit.
void show_template_browser(AppContext& context, bool for_new_project)
{
    if (for_new_project) context.session.new_from_template("cinematic.suspense.tension");
}

void show_chord_explorer(AppContext& context) { context.status("Chord Explorer is not built yet"); }
void show_ostinato_creator(AppContext& context) { context.status("Ostinato Creator is not built yet"); }

}  // namespace vcc::desktop
