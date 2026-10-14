--!strict
--- `LocalizationService` (ADR 0154) as a script meets it, over the two catalogs
--- in `tests/conformance/i18n`: a key by locale, placeholders, a key nobody has,
--- and the choosing of a locale.

local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect

local Localization = game:GetService("LocalizationService")

describe("LocalizationService", function()
    it("lists the locales the project has a catalog for, in order", function()
        local locales = Localization:GetLocales()
        expect(#locales):ToBe(2)
        expect(locales[1]):ToBe("en")
        expect(locales[2]):ToBe("pt-BR")
    end)

    it("reads a key in the locale that is set", function()
        Localization.Locale = "en"
        expect(Localization:Translate("spec.menu.play")):ToBe("Play")
        Localization.Locale = "pt-BR"
        expect(Localization.Locale):ToBe("pt-BR")
        expect(Localization:Translate("spec.menu.play")):ToBe("Jogar")
        -- What the locale lacks is read in the project's default.
        expect(Localization:Translate("spec.menu.quit")):ToBe("Quit")
    end)

    it("fills placeholders from the arguments", function()
        Localization.Locale = "en"
        expect(Localization:Translate("spec.hud.score", { points = 1200 })):ToBe("Score: 1200")
        expect(Localization:Translate("spec.hud.greeting", { name = "Ana" })):ToBe("Hello, Ana!")
        Localization.Locale = "pt-BR"
        expect(Localization:Translate("spec.hud.score", { points = 7 })):ToBe("Pontos: 7")
        expect(Localization:Translate("spec.hud.greeting", { name = "Ana" })):ToBe("Olá, Ana!")
    end)

    it("gives back the key when nobody has it, and never raises", function()
        expect(Localization:Translate("spec.nobody.has.this")):ToBe("spec.nobody.has.this")
        expect(Localization:Translate("spec.nobody.has.this", { count = 1 })):ToBe("spec.nobody.has.this")
    end)

    it("reads the engine's own text by the same call", function()
        expect(Localization:Translate("scene.err.number_positive")):ToBe("It takes a number greater than zero.")
    end)

    it("narrows a language to the catalog there is, and refuses one there is none for", function()
        Localization.Locale = "en"
        Localization.Locale = "pt"
        expect(Localization.Locale):ToBe("pt-BR")
        Localization.Locale = "EN_us"
        expect(Localization.Locale):ToBe("en")
        expect(function()
            Localization.Locale = "fr"
        end):ToThrow()
        expect(function()
            Localization.Locale = ""
        end):ToThrow()
        expect(Localization.Locale):ToBe("en")
    end)

    it("says LocaleChanged once for a change, and not for the same locale again", function()
        Localization.Locale = "en"
        task.wait()
        local heard: { string } = {}
        local connection = Localization.LocaleChanged:Connect(function(locale)
            table.insert(heard, locale)
        end)
        Localization.Locale = "pt-BR"
        Localization.Locale = "pt-BR"
        task.wait()
        task.wait()
        expect(#heard):ToBe(1)
        expect(heard[1]):ToBe("pt-BR")
        connection:Disconnect()
        Localization.Locale = "en"
    end)
end)
