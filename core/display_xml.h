#pragma once

#include "model.h"
#include <QJsonObject>
#include <array>

namespace recovery {

struct DisplayGroup {
    QString id;
    QString name;
    int parent = -1;
    QVector<int> parameters;
    QVector<int> childGroups;
};

struct DisplayLayout {
    // Index zero is the implicit root group. All other groups retain CDI order.
    QVector<DisplayGroup> groups;
    QVector<int> parameterParents;
    // The first parameter in an adjacent pair has combined=true in CMO3.
    QVector<bool> combined;
    QVector<std::array<int, 2>> combinedPairs;
};

// Restores the display hierarchy and verified two-parameter combination layout.
// Invalid references, cycles, and unsupported combinations are rejected.
DisplayLayout displayLayout(const QJsonObject& displayInfo,
                            const QVector<Parameter>& parameters);

}
