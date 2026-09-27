// A table handed to `print`, as the console shows it (the owner, 2026-09-27: a
// printed table opens and closes, and so do the tables inside it).
//
// Two forms of one value. The TEXT is what the log file and every sink read:
// one line, `{a = 1, b = {...}}`, cut short past a width, which is already
// more use than `table: 0x...`. The TREE travels beside it (`core::logDetail`)
// for the console to fold: one row a field, parents before children, each row
//
//     <depth> 0x1F <key> 0x1F <value> '\n'
//
// with the tables inside it as rows one deeper. Keys in a stable order --
// numbers ascending, then strings -- because iteration order is the table's
// and a list that shuffles between two prints of the same table reads as two
// different tables.
#pragma once

#include <string>

struct lua_State;

namespace engine::script {

// Whether `print` expands the value at `index`: a table with no `__tostring`.
// A table that says how it prints has chosen its text.
[[nodiscard]] bool printsAsTree(lua_State* L, int index);

// The one-line form of the table at `index`.
[[nodiscard]] std::string printSummary(lua_State* L, int index);

// Appends the rows of the table at `index`, its own fields at `depth`.
void appendPrintTree(lua_State* L, int index, int depth, std::string& out);

} // namespace engine::script
