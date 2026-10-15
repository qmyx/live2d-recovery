#pragma once
#include "model.h"

namespace recovery {
// Creates a self-contained Cubism project. Existing files are never replaced.
// Invalid or unsupported input is reported through std::runtime_error.
void writeProject(const Model& model, const QString& outputPath);
}
