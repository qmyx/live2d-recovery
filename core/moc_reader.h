#pragma once

#include "model.h"

namespace recovery {

// Reads the model manifest and its MOC3. Throws std::runtime_error on malformed
// or unsupported binary layouts. Feature gaps are reported through Model.
Model readModel(const QString& modelJson);

}
