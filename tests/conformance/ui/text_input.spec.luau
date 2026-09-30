--!strict
--- Conformance: a text input as a script sees it (ADR 0139) -- its properties,
--- their defaults and their limits, focus from code, and the query of which
--- field has the keyboard. What the keys and the mouse do is the engine's own
--- tests' (`engine/ui/tests`), since a script cannot press a key.

local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect

local function field(): (TextInput, ScreenGui)
    local screen = Instance.new("ScreenGui")
    screen.Parent = game:GetService("UIService")
    local input = Instance.new("TextInput")
    input.Size = UDim2.fromOffset(200, 30)
    input.Parent = screen
    return input, screen
end

describe("TextInput", function()
    it("starts as a single-line, editable field with nothing selected", function()
        local input, screen = field()
        expect(input.MultiLine):ToBe(false)
        expect(input.Editable):ToBe(true)
        expect(input.MaxLength):ToBe(0)
        expect(input.Masked):ToBe(false)
        expect(input.MaskCharacter):ToBe("\u{2022}")
        expect(input.ClearTextOnFocus):ToBe(false)
        expect(input.SelectAllOnFocus):ToBe(false)
        expect(input.ReleaseFocusOnSubmit):ToBe(true)
        expect(input.RevertOnEscape):ToBe(false)
        expect(input.KeyboardType):ToBe(Enum.TextInputKeyboard.Default)
        expect(input.CursorPosition):ToBe(-1)
        expect(input.SelectionStart):ToBe(-1)
        expect(input:IsFocused()):ToBe(false)
        screen:Destroy()
    end)

    it("keeps what it is given, and one character of a mask", function()
        local input, screen = field()
        input.MultiLine = true
        input.MaxLength = 12
        input.Masked = true
        input.MaskCharacter = "*#"
        input.KeyboardType = Enum.TextInputKeyboard.Email
        input.PlaceholderColor = Color3.fromRGB(10, 20, 30)
        expect(input.MultiLine):ToBe(true)
        expect(input.MaxLength):ToBe(12)
        expect(input.Masked):ToBe(true)
        expect(input.MaskCharacter):ToBe("*")
        expect(input.KeyboardType):ToBe(Enum.TextInputKeyboard.Email)
        expect(input.PlaceholderColor):ToBe(Color3.fromRGB(10, 20, 30))
        expect(function()
            input.MaxLength = -1
        end):ToThrow()
        screen:Destroy()
    end)

    it("names why focus went, and what keyboards a phone can raise", function()
        local reasons = {}
        for _, item in Enum.FocusLossReason:GetEnumItems() do
            table.insert(reasons, item.Name)
        end
        expect(table.concat(reasons, ",")):ToBe("Submitted,Moved,Cancelled,Script")
        expect(#Enum.TextInputKeyboard:GetEnumItems()):ToBe(6)
    end)

    it("takes and lets go of the keyboard when a script asks, at the next frame", function()
        local input, screen = field()
        local UIService = game:GetService("UIService")
        local seen = {}
        input.Focused:Connect(function()
            table.insert(seen, "focused")
        end)
        input.FocusLost:Connect(function(submitted: boolean, reason: EnumItem)
            table.insert(seen, `lost:{submitted}:{reason.Name}`)
        end)
        UIService.TextInputFocused:Connect(function(which: TextInput)
            table.insert(seen, `service-focused:{which == input}`)
        end)
        input.Text = "hello"
        input:CaptureFocus()
        task.wait()
        task.wait()
        expect(input:IsFocused()):ToBe(true)
        expect(UIService:GetFocusedTextInput()):ToBe(input)
        -- The caret at the end, as a Luau string's position after the last byte.
        expect(input.CursorPosition):ToBe(6)
        input.CursorPosition = 2
        input.SelectionStart = 4
        expect(input.CursorPosition):ToBe(2)
        expect(input.SelectionStart):ToBe(4)

        input:ReleaseFocus(true)
        task.wait()
        task.wait()
        expect(input:IsFocused()):ToBe(false)
        expect(UIService:GetFocusedTextInput()):ToBe(nil)
        expect(table.concat(seen, ",")):ToBe("focused,service-focused:true,lost:true:Script")
        screen:Destroy()
    end)
end)
