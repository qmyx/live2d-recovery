#include "display_xml.h"

#include <QHash>
#include <QJsonArray>
#include <QSet>
#include <stdexcept>

namespace recovery {
namespace {

[[noreturn]] void invalid(const QString& message)
{
    throw std::runtime_error((QStringLiteral("Display information: ") + message).toUtf8().constData());
}

QJsonArray arrayField(const QJsonObject& object, const QString& key)
{
    if (!object.contains(key)) return {};
    if (!object.value(key).isArray()) invalid(key + QStringLiteral(" must be an array."));
    return object.value(key).toArray();
}

QString stringField(const QJsonObject& object, const QString& key, bool required)
{
    if (!object.contains(key) && !required) return {};
    if (!object.value(key).isString()) invalid(key + QStringLiteral(" must be a string."));
    const auto value = object.value(key).toString();
    if (required && value.isEmpty()) invalid(key + QStringLiteral(" cannot be empty."));
    return value;
}

QJsonObject objectEntry(const QJsonValue& value, const QString& collection)
{
    if (!value.isObject()) invalid(collection + QStringLiteral(" entries must be objects."));
    return value.toObject();
}

}

DisplayLayout displayLayout(const QJsonObject& info, const QVector<Parameter>& parameters)
{
    DisplayLayout result;
    result.groups.append({QStringLiteral("ParamGroupRoot"), QStringLiteral("Parameters"), -1, {}, {}});
    result.parameterParents.fill(0, parameters.size());
    result.combined.fill(false, parameters.size());

    QHash<QString, int> parameterIndices;
    for (int index = 0; index < parameters.size(); ++index) {
        if (parameters[index].id.isEmpty() || parameterIndices.contains(parameters[index].id)) {
            invalid(QStringLiteral("model parameter identifiers must be unique and nonempty."));
        }
        parameterIndices.insert(parameters[index].id, index);
    }

    QHash<QString, int> groupIndices;
    groupIndices.insert(QString(), 0);
    const auto groups = arrayField(info, QStringLiteral("ParameterGroups"));
    QStringList parentIds;
    for (const auto& entry : groups) {
        const auto group = objectEntry(entry, QStringLiteral("ParameterGroups"));
        const auto id = stringField(group, QStringLiteral("Id"), true);
        if (id == result.groups[0].id || groupIndices.contains(id)) {
            invalid(QStringLiteral("parameter group identifiers must be unique and cannot use the root identifier."));
        }
        auto name = stringField(group, QStringLiteral("Name"), false);
        if (name.isEmpty()) name = id;
        groupIndices.insert(id, static_cast<int>(result.groups.size()));
        result.groups.append({id, name, 0, {}, {}});
        parentIds.append(stringField(group, QStringLiteral("GroupId"), false));
    }
    for (int index = 1; index < result.groups.size(); ++index) {
        const auto parent = parentIds[index - 1];
        if (!groupIndices.contains(parent)) invalid(QStringLiteral("a parameter group references a missing parent."));
        result.groups[index].parent = groupIndices[parent];
    }
    for (int index = 1; index < result.groups.size(); ++index) {
        QSet<int> ancestors;
        for (int parent = index; parent >= 0; parent = result.groups[parent].parent) {
            if (ancestors.contains(parent)) invalid(QStringLiteral("parameter group parents contain a cycle."));
            ancestors.insert(parent);
        }
        result.groups[result.groups[index].parent].childGroups.append(index);
    }

    QSet<int> listedParameters;
    for (const auto& entry : arrayField(info, QStringLiteral("Parameters"))) {
        const auto parameter = objectEntry(entry, QStringLiteral("Parameters"));
        const auto id = stringField(parameter, QStringLiteral("Id"), true);
        if (!parameterIndices.contains(id)) invalid(QStringLiteral("a display parameter is absent from the model."));
        const int index = parameterIndices[id];
        if (listedParameters.contains(index)) invalid(QStringLiteral("a display parameter is listed more than once."));
        const auto group = stringField(parameter, QStringLiteral("GroupId"), false);
        if (!groupIndices.contains(group)) invalid(QStringLiteral("a parameter references a missing group."));
        const int parent = groupIndices[group];
        listedParameters.insert(index);
        result.parameterParents[index] = parent;
        result.groups[parent].parameters.append(index);
    }
    for (int index = 0; index < parameters.size(); ++index) {
        if (!listedParameters.contains(index)) result.groups[0].parameters.append(index);
    }

    QSet<int> pairedParameters;
    for (const auto& entry : arrayField(info, QStringLiteral("CombinedParameters"))) {
        if (!entry.isArray() || entry.toArray().size() != 2) {
            invalid(QStringLiteral("only pairs of adjacent combined parameters are currently supported."));
        }
        const auto pair = entry.toArray();
        std::array<int, 2> indexes{};
        for (int axis = 0; axis < 2; ++axis) {
            if (!pair[axis].isString() || !parameterIndices.contains(pair[axis].toString())) {
                invalid(QStringLiteral("a combined parameter references a missing parameter."));
            }
            indexes[axis] = parameterIndices[pair[axis].toString()];
            if (pairedParameters.contains(indexes[axis])) {
                invalid(QStringLiteral("combined parameter pairs overlap or repeat a parameter."));
            }
            pairedParameters.insert(indexes[axis]);
        }
        if (result.parameterParents[indexes[0]] != result.parameterParents[indexes[1]]) {
            invalid(QStringLiteral("combined parameters must belong to the same group."));
        }
        const auto& ordered = result.groups[result.parameterParents[indexes[0]]].parameters;
        const auto first = ordered.indexOf(indexes[0]);
        const auto second = ordered.indexOf(indexes[1]);
        if (first < 0 || second != first + 1) {
            invalid(QStringLiteral("combined parameters must be adjacent and in the declared order."));
        }
        result.combined[indexes[0]] = true;
        result.combinedPairs.append(indexes);
    }
    return result;
}

}
