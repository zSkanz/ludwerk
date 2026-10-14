--!strict
--[[
	Every test that may skip for want of a graphics card is on the workflow's
	lists of tests allowed to.

	A test that draws answers `ENG_TEST_SKIP` on a machine with no graphics
	device, and CTest calls that a pass. `scripts/gates/assert-no-skips.sh`
	turns an unexpected skip into a failure, and `.github/workflows/ci.yml`
	hands it, three times -- once a job -- the names that MAY skip on a hosted
	runner. A new picture gate that is not on those lists passes on every
	machine with a graphics card, which is every machine a person builds on,
	and fails on the runner after the push: twice in one week, each a red
	branch and a wait.

	So the lists are checked against the tests themselves. The names come from
	CTest's own listing of a configured tree -- every test whose
	`SKIP_REGULAR_EXPRESSION` is `ENG_TEST_SKIP` -- and each has to be on every
	list, but for:

	  - a test labelled `gpu-golden`, which the workflow does not run at all;
	  - the few named below, each with why.

	    ctest --preset <preset> --show-only=json-v1 > listing.json
	    lute tools/repo/skiplists.luau listing.json
]]

local fs = require("@std/fs")
local json = require("@std/json")
local process = require("@std/process")

-- Another file to read the lists from, for trying this lint against a
-- workflow with a name taken out.
local WorkflowPath = process.env.ENG_SKIPLISTS_WORKFLOW or ".github/workflows/ci.yml"

-- Tests that carry the skip and are deliberately not on every list.
local Elsewhere: { [string]: string } = {
    -- Skips when the Luau tooling it drives is not installed, which on a
    -- runner is a broken runner and must fail the job.
    script_check_gate = "none",
    -- Registered on Windows alone, and listed for the job that builds there.
    window_hold_gate = "one",
}

local listing: string? = nil
for index, argument in process.args do
    if index > 1 and string.sub(argument, -5) == ".json" then
        listing = argument
    end
end
if listing == nil then
    print("skiplists: usage: lute tools/repo/skiplists.luau <ctest --show-only=json-v1 output>")
    process.exit(2)
    return
end

local decoded = json.deserialize(fs.readFileToString(listing :: string)) :: any
local skippable: { string } = {}
for _, test in decoded.tests :: { any } do
    local skips = false
    local golden = false
    for _, property in (test.properties or {}) :: { any } do
        if property.name == "SKIP_REGULAR_EXPRESSION" then
            local value = property.value
            if typeof(value) == "table" then
                for _, pattern in value :: { any } do
                    skips = skips or pattern == "ENG_TEST_SKIP"
                end
            else
                skips = value == "ENG_TEST_SKIP"
            end
        elseif property.name == "LABELS" then
            for _, label in property.value :: { any } do
                golden = golden or label == "gpu-golden"
            end
        end
    end
    if skips and not golden then
        table.insert(skippable, test.name :: string)
    end
end
table.sort(skippable)

local lists: { { line: number, names: { [string]: boolean } } } = {}
local lineNumber = 0
for line in string.gmatch(fs.readFileToString(WorkflowPath) .. "\n", "([^\n]*)\n") do
    lineNumber += 1
    local found = string.find(line, "assert-no-skips.sh ctest.log", 1, true)
    if found ~= nil then
        local names: { [string]: boolean } = {}
        for name in string.gmatch(string.sub(line, found + #"assert-no-skips.sh ctest.log"), "[%w_%-%.]+") do
            names[name] = true
        end
        table.insert(lists, { line = lineNumber, names = names })
    end
end

local problems: { string } = {}
if #lists == 0 then
    table.insert(problems, `{WorkflowPath}: no call of assert-no-skips.sh was found -- this lint stopped reading it`)
end
for _, name in skippable do
    local rule = Elsewhere[name]
    if rule == "none" then
        continue
    end
    local on = 0
    for _, list in lists do
        if list.names[name] then
            on += 1
        elseif rule == nil then
            table.insert(
                problems,
                `{WorkflowPath}:{list.line}: "{name}" may skip without a graphics device and is not on this list`
            )
        end
    end
    if rule == "one" and on == 0 then
        table.insert(problems, `{WorkflowPath}: "{name}" may skip without a graphics device and is on no list`)
    end
end

if #problems > 0 then
    for _, problem in problems do
        print(`skiplists: ERROR: {problem}`)
    end
    print("skiplists: add each name to the lists in the workflow, beside the gates already there.")
    process.exit(1)
end
print(`skiplists: ok -- {#skippable} test(s) that may skip, each on all {#lists} list(s) of the workflow`)
