package engine.player;

import android.content.Context;
import android.graphics.Insets;
import android.os.Build;
import android.view.View;
import android.view.WindowInsets;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

/**
 * SDL's surface, with one answer of its own: what the SAFE AREA is.
 *
 * <p>SDL's answer is the union of every inset the system has -- the bars, the
 * camera's cutout, and the strips along the edges where a swipe belongs to the
 * system (back from either side, home from the bottom, the bars from the top).
 * Those strips are on all four edges of every phone with gesture navigation,
 * so a game's interface lost about 24 dp on every side: 89 pixels left and
 * right on a phone held upright with nothing there to avoid, and the same
 * taken off the top of one lying down.
 *
 * <p>A gesture strip is not something a HUD must stay out of. It is glass like
 * the rest, it draws like the rest, and a game that runs with the bars hidden
 * is already asked for a second swipe before the system takes one. What is
 * NOT glass, or is drawn over, is the cutout and whichever bars are showing --
 * and that is the safe area every other engine reports too.
 */
public class PlayerSurface extends SDLSurface {

    public PlayerSurface(Context context) {
        super(context);
    }

    @Override
    public WindowInsets onApplyWindowInsets(View v, WindowInsets insets) {
        // SDL first: it also watches the on-screen keyboard come and go here.
        WindowInsets passed = super.onApplyWindowInsets(v, insets);
        if (Build.VERSION.SDK_INT >= 30 /* Android 11 (R) */) {
            // `getInsets` counts a bar only while it is showing, so with the
            // bars hidden this is the cutout alone -- one edge, the camera's.
            Insets kept = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
            // The last call is the one the window keeps.
            SDLActivity.onNativeInsetsChanged(kept.left, kept.right, kept.top, kept.bottom);
        }
        return passed;
    }
}
