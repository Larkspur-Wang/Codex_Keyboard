#pragma once

// Returns the address of an internal, never-executed probe table. The call is
// present only in the explicit assets diagnostic image so ESP-IDF must retain
// and fully link the storage core and platform adapter without reading or
// writing either sound bank at runtime.
extern "C" const void*
easy_input_speaker_assets_diagnostic_link_anchor();
