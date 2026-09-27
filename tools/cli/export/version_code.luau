--!strict
--[[
	`ludwerk build --bump-version-code` (ADR 0104 §3): `[export.android]
	version_code`, plus one, written back into `project.toml`.

	**Edited as lines, in place**, because the file is the author's: a
	re-serialisation would lose their comments and reorder their keys, and the
	one number that changed would be lost in a diff of the whole file. The rest
	of a `version_code` line -- a trailing comment -- is kept as it was.
]]

-- What `[export.android]` gives a game that names no code.
local DefaultCode = 1

--- The file with the code one higher, and that code.
local function bump(text: string): (string, number)
    local crlf = string.find(text, "\r\n", 1, true) ~= nil
    local newline = if crlf then "\r\n" else "\n"
    local lines = string.split((string.gsub(text, "\r\n", "\n")), "\n")
    local inSection = false
    local sectionLine: number? = nil
    for index, line in lines do
        local header = string.match(line, "^%s*%[([^%]]+)%]")
        if header ~= nil then
            if inSection then
                -- The section ends here and named no code: one, bumped.
                local at = index
                while at > (sectionLine :: number) + 1 and string.match(lines[at - 1], "^%s*$") do
                    at -= 1
                end
                table.insert(lines, at, `version_code = {DefaultCode + 1}`)
                return table.concat(lines, newline), DefaultCode + 1
            end
            inSection = string.match(header, "^%s*(.-)%s*$") == "export.android"
            if inSection then
                sectionLine = index
            end
        elseif inSection then
            local before, number, after = string.match(line, "^(%s*version_code%s*=%s*)(%d+)(.*)$")
            if before ~= nil then
                local code = (tonumber(number) :: number) + 1
                lines[index] = `{before}{code}{after}`
                return table.concat(lines, newline), code
            end
        end
    end
    local joined = table.concat(lines, newline)
    local code = DefaultCode + 1
    if inSection then
        -- The last section in the file, with no code in it.
        local trimmed = string.gsub(joined, "[\r\n]*$", "")
        return `{trimmed}{newline}version_code = {code}{newline}`, code
    end
    local trimmed = string.gsub(joined, "[\r\n]*$", "")
    return `{trimmed}{newline}{newline}[export.android]{newline}version_code = {code}{newline}`, code
end

return {
    bump = bump,
}
