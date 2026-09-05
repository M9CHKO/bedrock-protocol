package com.m9chko.bedrockrelay;

import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.view.Gravity;
import android.widget.Button;
import android.widget.TextView;

/** Shared visual language for the launcher and the in-game controls. */
final class RelayUi {
    static final int BACKGROUND = 0xff0c1018;
    static final int SURFACE = 0xff151c28;
    static final int RAISED = 0xff202b3b;
    static final int BORDER = 0xff2c394b;
    static final int TEXT = 0xffeff4fc;
    static final int MUTED = 0xffa4b2c6;
    static final int ACCENT = 0xff93b6ff;
    static final int SUCCESS = 0xff83dfb8;
    static final int WARNING = 0xffffcf83;
    static final int DANGER = 0xffffa6b3;

    private RelayUi() {}

    static int dp(Context context, int value) {
        return Math.round(value * context.getResources().getDisplayMetrics().density);
    }

    static GradientDrawable surface(Context context, int color, int radius, int border) {
        GradientDrawable drawable = new GradientDrawable();
        drawable.setColor(color);
        drawable.setCornerRadius(dp(context, radius));
        if (border != 0) drawable.setStroke(dp(context, 1), border);
        return drawable;
    }

    static RippleDrawable action(Context context, int color, int radius, int border) {
        return new RippleDrawable(ColorStateList.valueOf(0x3393b6ff),
            surface(context, color, radius, border), null);
    }

    static TextView text(Context context, String value, int size, boolean bold) {
        TextView view = new TextView(context);
        view.setText(value);
        view.setTextSize(size);
        view.setTextColor(TEXT);
        view.setFontFeatureSettings("kern");
        view.setTypeface(Typeface.create(bold ? "sans-serif-medium" : "sans-serif", Typeface.NORMAL));
        return view;
    }

    static Button button(Context context, String label, boolean primary) {
        Button button = new Button(context);
        button.setText(label);
        button.setTextSize(14);
        button.setAllCaps(false);
        button.setGravity(Gravity.CENTER);
        button.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        button.setTextColor(new ColorStateList(
            new int[][] {new int[] {-android.R.attr.state_enabled}, new int[0]},
            new int[] {MUTED, primary ? BACKGROUND : TEXT}));
        button.setBackground(action(context, primary ? ACCENT : RAISED, 16,
            primary ? 0 : BORDER));
        button.setMinHeight(dp(context, 48));
        button.setMinimumHeight(dp(context, 48));
        button.setPadding(dp(context, 12), dp(context, 10), dp(context, 12), dp(context, 10));
        button.setStateListAnimator(null);
        return button;
    }
}
