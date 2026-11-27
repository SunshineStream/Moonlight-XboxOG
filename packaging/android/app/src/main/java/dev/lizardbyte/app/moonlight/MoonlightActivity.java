package dev.lizardbyte.app.moonlight;

import android.os.Bundle;
import android.view.View;
import android.view.ViewGroup;

import org.libsdl.app.SDLActivity;

/** Holds the launch background over SDL's surface until Moonlight presents its first splash frame. */
public final class MoonlightActivity extends SDLActivity {
    private View startupCover;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        if (mLayout != null) {
            startupCover = new View(this);
            startupCover.setBackgroundColor(getColor(R.color.splash_background));
            mLayout.addView(startupCover, ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
        }
    }

    /** Called from the SDL thread after the splash framebuffer has been presented. */
    public void revealStartupSurface() {
        runOnUiThread(() -> {
            View cover = startupCover;
            startupCover = null;
            if (cover != null) {
                cover.postOnAnimation(() -> {
                    ViewGroup parent = (ViewGroup) cover.getParent();
                    if (parent != null) {
                        parent.removeView(cover);
                    }
                });
            }
        });
    }
}
