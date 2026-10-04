package engine.player;

import android.content.Context;
import android.content.Intent;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

/**
 * The activity a Ludwerk game runs in. SDL's own glue does the rest, compiled out
 * of the vendored tree (third_party/sdl3/android-project) rather than copied.
 *
 * <p>The same two overrides as the triangle sample's, for the same reasons: SDL
 * is linked statically into {@code libmain.so}, and the host has an ordinary
 * {@code main} rather than {@code SDL_main}.
 */
public class PlayerActivity extends SDLActivity {

    @Override
    protected String[] getLibraries() {
        return new String[] { "main" };
    }

    @Override
    protected String getMainFunction() {
        return "main";
    }

    /** The surface that says what the safe area is ({@link PlayerSurface}). */
    @Override
    protected SDLSurface createSDLSurface(Context context) {
        return new PlayerSurface(context);
    }

    /**
     * No arguments, as a rule: the host finds the packaged game beside itself
     * ({@code game/}, extracted from the APK on first launch), exactly as a
     * desktop build of a game does.
     *
     * <p>The exception is a measurement (ADR 0171): the launching intent's
     * {@code args} extra, split at spaces, is handed to the host as its
     * command line --
     * {@code adb shell am start -n <id>/engine.player.PlayerActivity --es args "--gpu-pass-times --hide=ui"}.
     * The host reads them only if the game's {@code project.toml} says
     * {@code [debug] launch_arguments = true}; here they are only passed on.
     */
    @Override
    protected String[] getArguments() {
        final Intent intent = getIntent();
        final String given = intent != null ? intent.getStringExtra("args") : null;
        if (given == null || given.trim().isEmpty()) {
            return new String[0];
        }
        return given.trim().split("\\s+");
    }
}
