-- Stride dropdown panel: a tab bar (Today / Upcoming / Inbox / Logbook), a
-- quick-add field scoped to whatever you're currently looking at, and the
-- list itself -- tap a task's circle to check it off, tap a folded project
-- row to drill into that project's own task list.
--
-- Nothing here is cached across opens: onOpen always re-fetches, so the
-- panel can never show something you already completed from the TUI.

local STRIDE = "stride"

local TABS = { "today", "upcoming", "inbox", "logbook" }
local TAB_LABELS = { today = "Today", upcoming = "Upcoming", inbox = "Inbox", logbook = "Logbook" }

local tab = "today"
local project = nil -- { id, name } while drilled into a project; nil at tab level
local items = {}
local loading = false
local draftText = ""
local inputGen = 0 -- bumped after a successful add, to force a fresh (empty) ui.input

-- Single-quotes a value for safe interpolation into the shell command
-- string noctalia.runAsync takes -- titles/list names can contain spaces,
-- quotes, anything.
local function shellQuote(s)
  return "'" .. tostring(s):gsub("'", "'\\''") .. "'"
end

local function render() end -- forward-declared; real definition below

local function loadTab()
  loading = true
  render()
  local cmd
  if project then
    cmd = STRIDE .. " --json project " .. shellQuote(tostring(project.id))
  else
    cmd = STRIDE .. " --json " .. tab
  end
  noctalia.runAsync(cmd, function(result)
    loading = false
    if result.exitCode ~= 0 then
      noctalia.notifyError("Stride", result.stderr ~= "" and result.stderr or "command failed")
      items = {}
      render()
      return
    end
    local data = noctalia.json.decode(result.stdout)
    items = project and (data and data.tasks or {}) or (data or {})
    render()
  end)
end

local function selectTab(name)
  return function()
    tab = name
    project = nil
    loadTab()
  end
end
local tabHandlers = {
  today = selectTab("today"),
  upcoming = selectTab("upcoming"),
  inbox = selectTab("inbox"),
  logbook = selectTab("logbook"),
}

local function backToTab()
  project = nil
  loadTab()
end

local function openProject(item)
  project = { id = item.id, name = item.title }
  loadTab()
end

local function completeItem(item)
  noctalia.runAsync(STRIDE .. " --complete " .. tostring(item.id) .. " --kind " .. item.kind, function(result)
    if result.exitCode ~= 0 then
      noctalia.notifyError("Stride", "couldn't complete that")
    end
    loadTab()
  end)
end

local function addTask(text)
  text = noctalia.string.trim(text or "")
  if text == "" then
    return
  end
  local cmd = STRIDE .. " --quick-add " .. shellQuote(text)
  if project then
    cmd = cmd .. " --list " .. shellQuote(project.name)
  elseif tab == "today" then
    cmd = cmd .. " --date today"
  end
  -- else: inbox / upcoming both just land undated in the Inbox, which is
  -- exactly what a bare add with no --list/--date does.
  noctalia.runAsync(cmd, function(result)
    if result.exitCode ~= 0 then
      noctalia.notifyError("Stride", "couldn't add that task")
    end
    loadTab()
  end)
  draftText = ""
  inputGen = inputGen + 1
end

local function formatMeta(item)
  local parts = {}
  if item.projectName ~= "" then
    table.insert(parts, item.projectName)
  elseif item.areaName ~= "" then
    table.insert(parts, item.areaName)
  end
  if item.deadline ~= "" then
    table.insert(parts, "due " .. item.deadline)
  end
  if tab ~= "today" and item.doDate ~= "" then
    table.insert(parts, item.doDate)
  end
  return table.concat(parts, "  -  ")
end

local function taskRow(item)
  if item.kind == "p" then
    return ui.row({ key = "p" .. item.id, gap = 8, align = "center", onClick = function() openProject(item) end }, {
      ui.glyph({ name = "folder", size = 14, color = "secondary" }),
      ui.label({ text = item.title, flexGrow = 1 }),
      ui.glyph({ name = "chevron-right", size = 14, color = "on_surface_variant" }),
    })
  end

  local meta = formatMeta(item)
  local labelChildren = { ui.label({ text = item.title, color = tab == "logbook" and "on_surface_variant" or "on_surface" }) }
  if meta ~= "" then
    table.insert(labelChildren, ui.label({ text = meta, fontSize = 11, color = "on_surface_variant" }))
  end

  return ui.row({ key = "t" .. item.id, gap = 8, align = "center" }, {
    ui.button({
      glyph = tab == "logbook" and "circle-check" or "circle",
      controlSize = "sm",
      variant = "ghost",
      enabled = tab ~= "logbook",
      onClick = function() completeItem(item) end,
    }),
    ui.column({ gap = 2, flexGrow = 1 }, labelChildren),
  })
end

local function headerRow()
  return ui.row({ align = "center", justify = "space_between" }, {
    ui.label({ text = "Stride", fontSize = 14, fontWeight = "bold" }),
    ui.button({ glyph = "x", variant = "ghost", controlSize = "sm", onClick = function() panel.close() end }),
  })
end

local function navRow()
  if project then
    return ui.row({ gap = 6, align = "center" }, {
      ui.button({ glyph = "arrow-left", variant = "ghost", controlSize = "sm", onClick = backToTab }),
      ui.label({ text = project.name, fontWeight = "bold", flexGrow = 1 }),
    })
  end
  local children = {}
  for _, t in ipairs(TABS) do
    table.insert(children, ui.button({
      key = "tab-" .. t,
      text = TAB_LABELS[t],
      controlSize = "sm",
      flexGrow = 1,
      variant = (tab == t) and "primary" or "ghost",
      onClick = tabHandlers[t],
    }))
  end
  return ui.row({ gap = 4 }, children)
end

local function addBar()
  if tab == "logbook" and not project then
    return nil -- nothing to add to in a read-only history view
  end
  local placeholder = "Add a task..."
  if project then
    placeholder = "Add to " .. project.name .. "..."
  elseif tab == "today" then
    placeholder = "Add to Today..."
  elseif tab == "inbox" then
    placeholder = "Add to Inbox..."
  end
  return ui.input({
    key = "add-" .. tab .. "-" .. (project and project.id or 0) .. "-" .. inputGen,
    placeholder = placeholder,
    controlSize = "sm",
    onChange = function(v) draftText = v end,
    onSubmit = function(v) addTask(v or draftText) end,
  })
end

render = function()
  local rows = { headerRow(), navRow() }

  local bar = addBar()
  if bar then
    table.insert(rows, bar)
  end

  local list = {}
  if #items == 0 then
    table.insert(list, ui.label({
      text = loading and "Loading..." or "Nothing here.",
      color = "on_surface_variant",
    }))
  else
    for _, item in ipairs(items) do
      table.insert(list, taskRow(item))
    end
  end
  table.insert(rows, ui.scroll({ gap = 6, flexGrow = 1 }, list))

  panel.render(ui.column({ gap = 10, padding = 12 }, rows))
end

function onOpen(context)
  tab = "today"
  project = nil
  items = {}
  loading = false
  draftText = ""
  loadTab()
end
