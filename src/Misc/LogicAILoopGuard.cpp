// AI-loop object guards and terrain/building throttles were removed.
//
// They skipped AI updates for selected objects, which left object state stale
// enough that later engine code dereferenced a null/dangling pointer and
// crashed (RUSATTA04). Per-frame AI is back to the original behaviour.
//
// This symbol is kept only for the diagnostics CSV column.

int LogicAIStats_LoopACount = 0;
