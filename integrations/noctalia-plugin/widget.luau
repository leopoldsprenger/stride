-- Stride bar widget: shows how many items are in Today, and opens the
-- dropdown (panel.luau) on click. Deliberately dumb -- all the real state
-- lives in the panel, which fetches fresh on every open, so this only ever
-- has to answer "how many today" for the badge.

local STRIDE = "stride"

local function refresh()
  noctalia.runAsync(STRIDE .. " --json today", function(result)
    barWidget.setGlyph("checklist")
    if result.exitCode ~= 0 then
      barWidget.setText("!")
      barWidget.setTooltip("stride --json today failed -- is stride on PATH and initialized?")
      return
    end
    local tasks = noctalia.json.decode(result.stdout) or {}
    local count = #tasks
    barWidget.setText(tostring(count))
    barWidget.setTooltip(count == 1 and "1 item in Today" or (tostring(count) .. " items in Today"))
  end)
end

-- Once a minute is plenty for a task count; the panel itself always
-- fetches fresh on open rather than relying on this cache.
noctalia.setUpdateInterval(60000)

function update()
  refresh()
end

function onClick()
  noctalia.togglePanel("leo/stride:panel")
end
