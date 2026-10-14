--!strict
--[[
	Prose lint: what a person reads about the engine says what a thing is and
	does, and says it in one spelling.

	Three places are read by somebody who is making a game, and never by us:
	the descriptions in the API definitions (which are the reference, the type
	file's comments and the editor's tooltips), the manual, and the message
	catalogs. None of them is the place for the project's own bookkeeping --
	the decision a property came from, the date a thing was added -- which is
	of no use to the person hovering over a field.

	What fails, with its place:

	  * **A reference to the project's records**: a decision's number, a
	    section mark, a work item's tag in brackets, a date. A definition
	    keeps those in `Records`, which nothing renders.
	  * **An empty description**, on anything that takes one.
	  * **A first sentence longer than 160 characters**, in a description: it
	    is what a tooltip shows, and it stands by itself.
	  * **A dash carrying a second sentence**, in a description or a catalog
	    message: ` -- ` and the long dash.
	  * **A word in the other spelling**: `spelling.luau` holds the list.
	  * **A tip the editor draws with the toolkit's own call**, in
	    `engine/app/src`: that call draws one line however long, and a
	    sentence wider than the window ran off its edge. `itemTip` and
	    `pointerTip` (`engine/app/tip.h`) cut the words into lines.

	Not read at all: `docs/decisions`, the changelog, comments and commit
	messages.

	    lute tools/repo/proselint.luau           check
	    lute tools/repo/proselint.luau --fix     write the spelling list into the manual and the catalogs
]]

local fs = require("@std/fs")
local path = require("@std/path")
local process = require("@std/process")

local Definition = require("../../api/defs")
local Spelling = require("./spelling")

local ManualRoot = "docs/manual"
local Catalogs = { "i18n/en.json", "tools/cli/i18n/en.json" }
local EditorSources = "engine/app/src"
--- The one file that may call the toolkit's tooltip: the helper that wraps it.
local TipHelper = "tip.cpp"
local FirstSentenceLimit = 160

--- What names one of the project's records. Lua patterns, read against text whose
--- code spans were taken out first: a code in backticks is somebody's value.
local RecordPatterns: { { Pattern: string, What: string } } = {
    { Pattern = "ADR%s*%d", What = "a decision record's number" },
    { Pattern = "§", What = "a section mark" },
    { Pattern = "%f[%w]20%d%d%-%d%d%-%d%d%f[%W]", What = "a date" },
}

type Finding = { Where: string, What: string }

local Findings: { Finding } = {}

local function report(where: string, what: string)
    table.insert(Findings, { Where = where, What = what })
end

--- `text` without what is code: a span between backticks, and a `{name}`
--- placeholder of a catalog message.
local function withoutCode(text: string): string
    local bare = string.gsub(text, "`[^`]*`", " ")
    bare = string.gsub(bare, "{[%w_]+}", " ")
    return bare
end

local function checkRecords(where: string, bare: string, tags: boolean?)
    -- A work item's tag in brackets, ``: only where no key is ever named, since
    -- `(F5)` in a tooltip is a key.
    if tags == true then
        local tag = string.match(bare, "%([FGHNV]%d+%)")
        if tag ~= nil then
            report(where, `names a work item's tag ("{tag}")`)
        end
    end
    for _, entry in RecordPatterns do
        local found = string.match(bare, entry.Pattern)
        if found ~= nil then
            report(where, `names {entry.What} ("{found}")`)
        end
    end
end

local function checkSpelling(where: string, bare: string)
    local seen: { [string]: boolean } = {}
    for word in string.gmatch(bare, "%a+") do
        local lower = string.lower(word)
        local american = Spelling[lower]
        if american ~= nil and not seen[lower] then
            seen[lower] = true
            report(where, `"{word}" is written "{american}"`)
        end
    end
end

local function checkDashes(where: string, bare: string)
    if string.find(bare, " %-%- ") ~= nil or string.find(bare, "—", 1, true) ~= nil then
        report(where, "a dash carries a second sentence; write two sentences")
    end
end

--- The first sentence of a description, as a tooltip shows it: the first
--- paragraph up to a full stop that a new sentence follows, without the marks.
local function firstSentence(doc: string): string
    local paragraph = string.match(doc, "^(.-)\n\n") or doc
    local sentence = paragraph
    local from = 1
    while true do
        local stop = string.find(paragraph, "[.!?]", from)
        if stop == nil then
            break
        end
        -- A name in code may begin a sentence, so the backtick counts.
        local after = string.sub(paragraph, stop + 1, stop + 2)
        if after == "" or string.match(after, "^%s[%u%d\"'(`]") ~= nil then
            sentence = string.sub(paragraph, 1, stop)
            break
        end
        from = stop + 1
    end
    return (string.gsub(sentence, "[`*]", ""))
end

-- The definitions -----------------------------------------------------------

local Described = 0

--- Every `Doc` under `node`, each with the names that lead to it. The
--- definition is data and is walked as data: a kind of member added tomorrow
--- is read without this file hearing about it.
local function walk(node: any, where: string)
    if type(node) ~= "table" then
        return
    end
    local here = where
    if type(node.Name) == "string" then
        here = if where == "" then node.Name else `{where}.{node.Name}`
    end
    local doc = node.Doc
    if type(doc) == "string" then
        Described += 1
        if string.match(doc, "^%s*$") ~= nil then
            report(here, "has no description")
        else
            local bare = withoutCode(doc)
            checkRecords(here, bare, true)
            checkDashes(here, bare)
            checkSpelling(here, bare)
            local first = firstSentence(doc)
            if #first > FirstSentenceLimit then
                report(here, `its first sentence is {#first} characters; a tooltip shows {FirstSentenceLimit}`)
            end
        end
    end
    for key, child in node do
        if type(child) == "table" and key ~= "Records" then
            walk(child, here)
        end
    end
end

-- Files ---------------------------------------------------------------------

local function filesUnder(root: string, suffix: string): { string }
    local found: { string } = {}
    local pending: { string } = { root }
    while #pending > 0 do
        local dir = table.remove(pending) :: string
        for _, entry in fs.listDirectory(dir) do
            local child = string.gsub(path.format(path.join(dir, entry.name)), "\\", "/")
            if entry.type == "dir" then
                table.insert(pending, child)
            elseif entry.type == "file" and string.sub(child, -#suffix) == suffix then
                table.insert(found, child)
            end
        end
    end
    table.sort(found)
    return found
end

local function linesOf(text: string): { string }
    local lines: { string } = {}
    for line in string.gmatch(text .. "\n", "([^\n]*)\n") do
        table.insert(lines, line)
    end
    -- The split adds one empty line after a final newline.
    if lines[#lines] == "" then
        table.remove(lines)
    end
    return lines
end

--- A word of the list, in the capitals it was written with.
local function respelled(word: string): string
    local american = Spelling[string.lower(word)]
    if american == nil then
        return word
    end
    if word == string.upper(word) and #word > 1 then
        return string.upper(american)
    end
    if string.match(word, "^%u") ~= nil then
        return string.upper(string.sub(american, 1, 1)) .. string.sub(american, 2)
    end
    return american
end

--- `line` with the list applied to its prose and to nothing else: a code
--- span, a link's target, a placeholder and an address are left as written.
local function respellLine(line: string): string
    local out: { string } = {}
    local at = 1
    while at <= #line do
        local from, to
        for _, guard in { "`[^`]*`", "%]%b()", "{[%w_]+}", "%a+://%S+", "<[^>]*>" } do
            local guardFrom, guardTo = string.find(line, guard, at)
            if guardFrom ~= nil and (from == nil or guardFrom < from) then
                from, to = guardFrom, guardTo
            end
        end
        local prose = string.sub(line, at, if from ~= nil then from - 1 else #line)
        table.insert(out, (string.gsub(prose, "%a+", respelled)))
        if from == nil or to == nil then
            break
        end
        table.insert(out, string.sub(line, from, to))
        at = (to :: number) + 1
    end
    return table.concat(out)
end

local Fix = false
for _, argument in process.args do
    if argument == "--fix" then
        Fix = true
    end
end

local Rewritten = 0

local function finish(file: string, original: string, lines: { string })
    local text = table.concat(lines, "\n") .. "\n"
    if Fix and text ~= original then
        fs.writeStringToFile(file, text)
        Rewritten += 1
    end
end

-- The manual ----------------------------------------------------------------

local function readManualPage(file: string)
    local original = fs.readFileToString(file)
    local lines = linesOf(original)
    local fenced = false
    for index, line in lines do
        if string.match(line, "^%s*```") ~= nil then
            fenced = not fenced
            continue
        end
        if fenced then
            continue
        end
        if Fix then
            lines[index] = respellLine(line)
            continue
        end
        local bare = withoutCode(line)
        bare = string.gsub(bare, "%]%b()", "]")
        bare = string.gsub(bare, "%a+://%S+", " ")
        bare = string.gsub(bare, "<[^>]*>", " ")
        checkRecords(`{file}:{index}`, bare)
        checkSpelling(`{file}:{index}`, bare)
    end
    finish(file, original, lines)
end

-- The catalogs --------------------------------------------------------------

local function readCatalog(file: string)
    if not fs.exists(file) then
        return
    end
    local original = fs.readFileToString(file)
    local lines = linesOf(original)
    for index, line in lines do
        local head, key, value, tail = string.match(line, '^(%s*"([^"]+)":%s*")(.*)("%s*,?%s*)$')
        if head == nil or key == nil or value == nil or tail == nil or string.sub(key, 1, 1) == "$" then
            continue
        end
        if Fix then
            lines[index] = head .. respellLine(value) .. tail
            continue
        end
        local where = `{file}:{index} ({key})`
        local bare = withoutCode(value)
        checkRecords(where, bare)
        checkDashes(where, bare)
        checkSpelling(where, bare)
    end
    finish(file, original, lines)
end

--- The editor's sources, for a tip drawn past the helper that wraps it.
local function readEditorSource(file: string)
    if string.sub(file, -#TipHelper - 1) == "/" .. TipHelper then
        return
    end
    for index, line in linesOf(fs.readFileToString(file)) do
        if string.find(line, "ImGui::SetItemTooltip", 1, true) or string.find(line, "ImGui::SetTooltip", 1, true) then
            report(
                `{file}:{index}`,
                "draws a tip with the toolkit's own call, which does not wrap: use itemTip or pointerTip"
            )
        end
    end
end

-- ---------------------------------------------------------------------------

if not Fix then
    walk(Definition, "")
    for _, file in filesUnder(EditorSources, ".cpp") do
        readEditorSource(file)
    end
end
local Pages = filesUnder(ManualRoot, ".md")
for _, file in Pages do
    readManualPage(file)
end
for _, file in Catalogs do
    readCatalog(file)
end

if Fix then
    print(`proselint: rewrote {Rewritten} file(s) from the spelling list`)
    process.exit(0)
end

if #Findings > 0 then
    table.sort(Findings, function(a: Finding, b: Finding): boolean
        if a.Where ~= b.Where then
            return a.Where < b.Where
        end
        return a.What < b.What
    end)
    for _, finding in Findings do
        print(`proselint: {finding.Where}: {finding.What}`)
    end
    print(`proselint: {#Findings} finding(s)`)
    process.exit(1)
end

print(`proselint: ok -- {Described} description(s), {#Pages} manual page(s), {#Catalogs} catalog(s)`)
