#pragma once
// Small scriptable surface on top of Store, for things that want Stride's
// data without a terminal -- e.g. `stride --json today` for a bar widget's
// dropdown, or `--complete`/`--quick-add` for its checkboxes and add-task
// field. Deliberately just these five verbs, each doing one obvious thing;
// this is not meant to grow into a general query language. See the
// "Bar widget" section of README.md for the shapes each one prints.
#include <string>

#include "store.h"

// Prints one of a handful of named views as a JSON array of task/project
// objects to stdout. `view` is one of: today, upcoming, inbox, logbook,
// projects, project. `arg` is the project name/id, required (and only
// used) for `view == "project"`. Returns 0, or 1 with a message on stderr
// if `view` isn't recognized or `project` couldn't be resolved.
int runJson(Store& s, const std::string& view, const std::string& arg);

// Marks a task (kind 't', the default) or project (kind 'p') done. Returns
// 0 on success, 1 (with a stderr message) if `id` doesn't resolve to an
// open item of that kind.
int runComplete(Store& s, int id, char kind);

// Adds a single task titled `title` with no description, and prints its
// new id as JSON ({"id": N}) so a caller can act on it further (e.g.
// complete it right back off). `list`, if non-empty, is an existing area
// or project name the task is filed under; otherwise it lands in the
// Inbox. `date` is a YYYY-MM-DD do date, the literal "today", or empty for
// unscheduled -- exactly what an Inbox/Today/project quick-add bar needs
// and nothing more. Returns 1 (with a stderr message) if `title` is blank.
int runQuickAdd(Store& s, const std::string& title, const std::string& list, const std::string& date);
