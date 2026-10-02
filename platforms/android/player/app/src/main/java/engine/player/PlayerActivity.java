package engine.player;

import android.content.Context;

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
     * No arguments: the host finds the packaged game beside itself
     * ({@code game/}, extracted from the APK on first launch), exactly as a
     * desktop build of a game does.
     */
    @Override
    protected String[] getArguments() {
        return new String[0];
    }
}
