--!strict
--- ADR 0128's classes as a script meets them: their defaults, what they
--- refuse, where they may be parented, a page layout's turning, and the
--- selection. Where things are laid out is `engine/ui/tests/`'s, in numbers.

local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect

local UIService = game:GetService("UIService")

describe("UIGridLayout", function()
    it("starts as a grid of 100 by 100 cells, 5 apart, filling rows from the top left", function()
        local grid = Instance.new("UIGridLayout")
        expect(grid.CellSize):ToBe(UDim2.new(0, 100, 0, 100))
        expect(grid.CellPadding):ToBe(UDim2.new(0, 5, 0, 5))
        expect(grid.FillDirection):ToBe(Enum.FillDirection.Horizontal)
        expect(grid.FillDirectionMaxCells):ToBe(0)
        expect(grid.StartCorner):ToBe(Enum.StartCorner.TopLeft)
        expect(grid.SortOrder):ToBe(Enum.SortOrder.LayoutOrder)
        expect(grid.HorizontalAlignment):ToBe(Enum.HorizontalAlignment.Left)
        expect(grid.VerticalAlignment):ToBe(Enum.VerticalAlignment.Top)
        expect(grid.AbsoluteContentSize):ToBe(Vector2.new(0, 0))
        grid:Destroy()
    end)

    it("refuses a count of cells that is not a whole number, and a write to what it measured", function()
        local grid = Instance.new("UIGridLayout")
        grid.FillDirectionMaxCells = 4
        expect(grid.FillDirectionMaxCells):ToBe(4)
        expect(function()
            grid.FillDirectionMaxCells = 2.5
        end):ToThrow()
        expect(function()
            grid.FillDirectionMaxCells = -1
        end):ToThrow()
        expect(function()
            (grid :: any).AbsoluteContentSize = Vector2.new(1, 1)
        end):ToThrow()
        grid:Destroy()
    end)
end)

describe("UIPageLayout", function()
    local function pagesIn(count: number): (Frame, UIPageLayout, { Frame })
        local holder = Instance.new("Frame")
        local layout = Instance.new("UIPageLayout")
        layout.Animated = false
        layout.Parent = holder
        local pages = {}
        for index = 1, count do
            local page = Instance.new("Frame")
            page.Name = `Page{index}`
            page.LayoutOrder = index
            page.Parent = holder
            table.insert(pages, page)
        end
        return holder, layout, pages
    end

    it("starts animated, not circular, taking every way of turning a page", function()
        local layout = Instance.new("UIPageLayout")
        expect(layout.Animated):ToBe(true)
        expect(layout.Circular):ToBe(false)
        expect(layout.EasingStyle):ToBe(Enum.EasingStyle.Quad)
        expect(layout.EasingDirection):ToBe(Enum.EasingDirection.Out)
        expect(layout.TweenTime):ToBeCloseTo(0.3, 1e-5)
        expect(layout.FillDirection):ToBe(Enum.FillDirection.Horizontal)
        expect(layout.ScrollWheelInputEnabled):ToBe(true)
        expect(layout.TouchInputEnabled):ToBe(true)
        expect(layout.GamepadInputEnabled):ToBe(true)
        expect(layout.CurrentPage):ToBe(nil)
        layout:Destroy()
    end)

    it("turns to the next page and the one before, and stops at either end", function()
        local holder, layout, pages = pagesIn(3)
        layout:Next()
        expect(layout.CurrentPage):ToBe(pages[2])
        layout:Next()
        expect(layout.CurrentPage):ToBe(pages[3])
        layout:Next()
        expect(layout.CurrentPage):ToBe(pages[3])
        layout:Previous()
        expect(layout.CurrentPage):ToBe(pages[2])
        layout:JumpToIndex(0)
        expect(layout.CurrentPage):ToBe(pages[1])
        layout:Previous()
        expect(layout.CurrentPage):ToBe(pages[1])
        holder:Destroy()
    end)

    it("goes round when it is circular", function()
        local holder, layout, pages = pagesIn(3)
        layout.Circular = true
        layout:Previous()
        expect(layout.CurrentPage):ToBe(pages[3])
        layout:Next()
        expect(layout.CurrentPage):ToBe(pages[1])
        layout:JumpToIndex(4)
        expect(layout.CurrentPage):ToBe(pages[2])
        holder:Destroy()
    end)

    it("jumps to a page, and refuses what is not one of its pages", function()
        local holder, layout, pages = pagesIn(3)
        layout:JumpTo(pages[3])
        expect(layout.CurrentPage):ToBe(pages[3])
        local stranger = Instance.new("Frame")
        expect(function()
            layout:JumpTo(stranger)
        end):ToThrow()
        expect(layout.CurrentPage):ToBe(pages[3])
        stranger:Destroy()
        holder:Destroy()
    end)

    it("says which page was left and which was entered, and that it stopped", function()
        local holder, layout, pages = pagesIn(2)
        -- The first page is turned to before anybody listens: what a layout
        -- says about that has been said by the time the listeners are there.
        layout:JumpToIndex(0)
        task.wait()
        local said: { string } = {}
        layout.PageLeave:Connect(function(page)
            table.insert(said, `leave {page.Name}`)
        end)
        layout.PageEnter:Connect(function(page)
            table.insert(said, `enter {page.Name}`)
        end)
        layout.Stopped:Connect(function(page)
            table.insert(said, `stopped {page.Name}`)
        end)
        layout:Next()
        task.wait()
        expect(#said):ToBe(3)
        expect(said[1]):ToBe(`leave {pages[1].Name}`)
        expect(said[2]):ToBe(`enter {pages[2].Name}`)
        expect(said[3]):ToBe(`stopped {pages[2].Name}`)
        holder:Destroy()
    end)

    it("orders its pages by LayoutOrder", function()
        local holder, layout, pages = pagesIn(3)
        pages[3].LayoutOrder = -1
        layout:JumpToIndex(0)
        expect(layout.CurrentPage):ToBe(pages[3])
        holder:Destroy()
    end)
end)

describe("flex in a UIListLayout", function()
    it("is off until it is asked for", function()
        local list = Instance.new("UIListLayout")
        expect(list.HorizontalFlex):ToBe(Enum.UIFlexAlignment.None)
        expect(list.VerticalFlex):ToBe(Enum.UIFlexAlignment.None)
        expect(list.ItemLineAlignment):ToBe(Enum.ItemLineAlignment.Automatic)
        expect(list.AbsoluteContentSize):ToBe(Vector2.new(0, 0))
        list.HorizontalFlex = Enum.UIFlexAlignment.SpaceBetween
        list.ItemLineAlignment = Enum.ItemLineAlignment.Stretch
        expect(list.HorizontalFlex):ToBe(Enum.UIFlexAlignment.SpaceBetween)
        expect(list.ItemLineAlignment):ToBe(Enum.ItemLineAlignment.Stretch)
        list:Destroy()
    end)

    it("a UIFlexItem goes under the object it is about", function()
        local frame = Instance.new("Frame")
        local item = Instance.new("UIFlexItem")
        expect(item.FlexMode):ToBe(Enum.UIFlexMode.None)
        expect(item.GrowRatio):ToBe(1)
        expect(item.ShrinkRatio):ToBe(1)
        expect(item.ItemLineAlignment):ToBe(Enum.ItemLineAlignment.Automatic)
        item.Parent = frame
        item.FlexMode = Enum.UIFlexMode.Custom
        item.GrowRatio = 3
        expect(item.GrowRatio):ToBe(3)
        expect(function()
            item.GrowRatio = -1
        end):ToThrow()
        frame:Destroy()
    end)
end)

describe("the constraints", function()
    it("a UIScale starts at one and is never negative", function()
        local scale = Instance.new("UIScale")
        expect(scale.Scale):ToBe(1)
        scale.Scale = 1.5
        expect(scale.Scale):ToBe(1.5)
        expect(function()
            scale.Scale = -0.5
        end):ToThrow()
        scale:Destroy()
    end)

    it("an aspect ratio is above zero", function()
        local shape = Instance.new("UIAspectRatioConstraint")
        expect(shape.AspectRatio):ToBe(1)
        expect(shape.AspectType):ToBe(Enum.AspectType.FitWithinMaxSize)
        expect(shape.DominantAxis):ToBe(Enum.DominantAxis.Width)
        shape.AspectRatio = 16 / 9
        expect(shape.AspectRatio):ToBeCloseTo(16 / 9, 1e-5)
        expect(function()
            shape.AspectRatio = 0
        end):ToThrow()
        shape:Destroy()
    end)

    it("a size constraint's maximum of zero is no limit, and infinity is the same", function()
        local limit = Instance.new("UISizeConstraint")
        expect(limit.MinSize):ToBe(Vector2.new(0, 0))
        expect(limit.MaxSize):ToBe(Vector2.new(0, 0))
        limit.MaxSize = Vector2.new(math.huge, 200)
        expect(limit.MaxSize):ToBe(Vector2.new(0, 200))
        expect(function()
            limit.MaxSize = Vector2.new(-1, 0)
        end):ToThrow()
        limit:Destroy()
    end)

    it("a text size constraint starts from 1 to 100", function()
        local limit = Instance.new("UITextSizeConstraint")
        expect(limit.MinTextSize):ToBe(1)
        expect(limit.MaxTextSize):ToBe(100)
        limit:Destroy()
    end)
end)

describe("CanvasGroup", function()
    it("is a frame, solid and untinted", function()
        local group = Instance.new("CanvasGroup")
        expect(group:IsA("Frame")):ToBe(true)
        expect(group:IsA("UIObject")):ToBe(true)
        expect(group.GroupTransparency):ToBe(0)
        expect(group.GroupColor):ToBe(Color3.new(1, 1, 1))
        group.GroupTransparency = 0.5
        expect(group.GroupTransparency):ToBe(0.5)
        group:Destroy()
    end)
end)

describe("UIDragDetector", function()
    it("starts enabled, moving its parent on both axes by its offset", function()
        local detector = Instance.new("UIDragDetector")
        expect(detector.Enabled):ToBe(true)
        expect(detector.DragStyle):ToBe(Enum.UIDragDetectorDragStyle.TranslatePlane)
        expect(detector.ResponseStyle):ToBe(Enum.UIDragDetectorResponseStyle.Offset)
        expect(detector.DragAxis):ToBe(Vector2.new(1, 0))
        expect(detector.BoundingUI):ToBe(nil)
        expect(detector.DragUDim2):ToBe(UDim2.new())
        expect(detector.DragRotation):ToBe(0)
        detector:Destroy()
    end)

    it("is bound by another object of the interface, and by nothing else", function()
        local frame = Instance.new("Frame")
        local track = Instance.new("Frame")
        local detector = Instance.new("UIDragDetector")
        detector.Parent = frame
        detector.BoundingUI = track
        expect(detector.BoundingUI):ToBe(track)
        expect(function()
            (detector :: any).BoundingUI = Instance.new("Part")
        end):ToThrow()
        detector.BoundingUI = nil
        expect(detector.BoundingUI):ToBe(nil)
        track:Destroy()
        frame:Destroy()
    end)
end)

describe("selection", function()
    it("a button and a text input are selectable, and a frame is when it is told", function()
        local button = Instance.new("TextButton")
        local field = Instance.new("TextInput")
        local frame = Instance.new("Frame")
        expect(button.Selectable):ToBe(true)
        expect(field.Selectable):ToBe(true)
        expect(frame.Selectable):ToBe(false)
        frame.Selectable = true
        button.Selectable = false
        expect(frame.Selectable):ToBe(true)
        expect(button.Selectable):ToBe(false)
        button:Destroy()
        field:Destroy()
        frame:Destroy()
    end)

    it("UIService.SelectedObject takes a selectable object, nothing, and refuses the rest", function()
        local screen = Instance.new("ScreenGui")
        screen.Parent = UIService
        local button = Instance.new("TextButton")
        button.Parent = screen
        local label = Instance.new("TextLabel")
        label.Parent = screen

        expect(UIService.AutoSelect):ToBe(true)
        UIService.SelectedObject = button
        expect(UIService.SelectedObject):ToBe(button)
        expect(function()
            UIService.SelectedObject = label
        end):ToThrow()
        expect(UIService.SelectedObject):ToBe(button)
        UIService.SelectedObject = nil
        expect(UIService.SelectedObject):ToBe(nil)

        -- What is selected and then destroyed is not selected.
        UIService.SelectedObject = button
        button:Destroy()
        expect(UIService.SelectedObject):ToBe(nil)
        screen:Destroy()
    end)

    it("an object says where the selection goes from it", function()
        local first = Instance.new("TextButton")
        local second = Instance.new("TextButton")
        expect(first.NextSelectionDown):ToBe(nil)
        first.NextSelectionDown = second
        second.NextSelectionUp = first
        expect(first.NextSelectionDown):ToBe(second)
        expect(second.NextSelectionUp):ToBe(first)
        expect(function()
            (first :: any).NextSelectionLeft = Instance.new("Part")
        end):ToThrow()
        first:Destroy()
        second:Destroy()
    end)
end)

describe("TextLabel.TextOverflow", function()
    it("is shown past its box until asked otherwise, and is one of three", function()
        local label = Instance.new("TextLabel")
        expect(label.TextOverflow):ToBe(Enum.TextOverflow.Overflow)
        label.TextOverflow = Enum.TextOverflow.Ellipsis
        expect(label.TextOverflow):ToBe(Enum.TextOverflow.Ellipsis)
        label.TextOverflow = Enum.TextOverflow.Clip
        expect(label.TextOverflow):ToBe(Enum.TextOverflow.Clip)
        expect(function()
            (label :: any).TextOverflow = Enum.HorizontalAlignment.Left
        end):ToThrow()
        -- What a script wrote is what it reads: the cut is in the drawing.
        label.Size = UDim2.fromOffset(40, 20)
        label.Text = "Wolfgang Amadeus Mozart the Second"
        label.TextOverflow = Enum.TextOverflow.Ellipsis
        expect(label.Text):ToBe("Wolfgang Amadeus Mozart the Second")
        -- A button is a label.
        local button = Instance.new("TextButton")
        button.TextOverflow = Enum.TextOverflow.Ellipsis
        expect(button.TextOverflow):ToBe(Enum.TextOverflow.Ellipsis)
        label:Destroy()
        button:Destroy()
    end)
end)
