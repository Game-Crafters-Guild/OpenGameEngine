#include "UI/UiDispatcher.h"

// No global dispatcher; UiContext (TLS) + each element's post route (UI::UiPostTarget) cover routing.
