package com.m9chko.bedrockrelay;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.RectF;
import android.graphics.drawable.Drawable;
import android.os.SystemClock;
import android.provider.Settings;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.Locale;

/** Click-through top-right HUD for the local player's equipment. */
final class EquipmentOverlayController {
    private final Context context;
    private final WindowManager windowManager;
    private boolean sessionVisible;
    private boolean enabled = true;
    private int scalePercent = 100;
    private EquipmentView view;
    private long lastRevision = Long.MIN_VALUE;
    private EquipmentItem[] latestItems;

    EquipmentOverlayController(Context context) {
        this.context = context;
        this.windowManager = (WindowManager) context.getSystemService(
            Context.WINDOW_SERVICE
        );
    }

    void setSessionVisible(boolean visible) {
        sessionVisible = visible;
        reconcile();
    }

    void configure(boolean show, int scale) {
        int clamped = RelayService.clampOverlayScale(scale);
        boolean recreate = scalePercent != clamped;
        enabled = show;
        scalePercent = clamped;
        if (recreate) removeWindow();
        reconcile();
    }

    void update(JSONArray equipment, long revision) {
        if (revision == lastRevision) return;
        lastRevision = revision;
        EquipmentItem[] items = new EquipmentItem[5];
        for (int index = 0; index < items.length; ++index) {
            items[index] = EquipmentItem.empty(index);
        }
        if (equipment != null) {
            for (int index = 0; index < equipment.length(); ++index) {
                JSONObject value = equipment.optJSONObject(index);
                if (value == null) continue;
                int slot = slotIndex(value.optString("slot", ""));
                if (slot < 0) continue;
                items[slot] = EquipmentItem.from(slot, value);
            }
        }
        latestItems = items;
        if (view != null) view.setItems(items);
    }

    void hideImmediately() {
        sessionVisible = false;
        removeWindow();
    }

    private void reconcile() {
        if (sessionVisible && enabled) addWindow();
        else removeWindow();
    }

    private void addWindow() {
        if (view != null || !Settings.canDrawOverlays(context)) return;
        EquipmentView added = new EquipmentView(context, scalePercent);
        WindowManager.LayoutParams params = new WindowManager.LayoutParams(
            added.preferredWidth(),
            added.preferredHeight(),
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE |
                WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE |
                WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL |
                WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
            PixelFormat.TRANSLUCENT
        );
        params.gravity = Gravity.TOP | Gravity.END;
        params.x = dp(12);
        params.y = dp(82);
        try {
            windowManager.addView(added, params);
            view = added;
            if (latestItems != null) added.setItems(latestItems);
        } catch (Throwable error) {
            DiagnosticsLog.appendError(
                context,
                "equipment",
                "Failed to show equipment HUD",
                error
            );
        }
    }

    private void removeWindow() {
        EquipmentView current = view;
        view = null;
        if (current == null) return;
        try {
            windowManager.removeViewImmediate(current);
        } catch (Throwable ignored) {
        }
    }

    private int dp(int value) {
        return Math.round(
            value * context.getResources().getDisplayMetrics().density
        );
    }

    private static int slotIndex(String slot) {
        switch (slot) {
            case "hand": return 0;
            case "helmet": return 1;
            case "chestplate": return 2;
            case "leggings": return 3;
            case "boots": return 4;
            default: return -1;
        }
    }

    private static final class EquipmentItem {
        final int slot;
        final boolean present;
        final String name;
        final int damage;
        final boolean enchanted;

        EquipmentItem(
            int slot,
            boolean present,
            String name,
            int damage,
            boolean enchanted
        ) {
            this.slot = slot;
            this.present = present;
            this.name = name;
            this.damage = Math.max(0, damage);
            this.enchanted = enchanted;
        }

        static EquipmentItem empty(int slot) {
            return new EquipmentItem(slot, false, "", 0, false);
        }

        static EquipmentItem from(int slot, JSONObject value) {
            return new EquipmentItem(
                slot,
                value.optBoolean("present", false),
                value.optString("name", ""),
                value.optInt("damage", 0),
                value.optBoolean("enchanted", false)
            );
        }
    }

    private static final class EquipmentView extends View {
        private static final String[] SLOT_NAMES = {
            "РУКА", "ШЛЕМ", "НАГРУДНИК", "ПОНОЖИ", "БОТИНКИ"
        };
        private static final int[] ICONS = {
            R.drawable.ic_equipment_hand,
            R.drawable.ic_equipment_helmet,
            R.drawable.ic_equipment_chest,
            R.drawable.ic_equipment_leggings,
            R.drawable.ic_equipment_boots
        };

        private final float density;
        private final float scale;
        private final int width;
        private final int height;
        private final Paint backgroundPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint borderPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint titlePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint barPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF rectangle = new RectF();
        private final Drawable[] icons = new Drawable[5];
        private EquipmentItem[] items = new EquipmentItem[5];

        EquipmentView(Context context, int scalePercent) {
            super(context);
            density = context.getResources().getDisplayMetrics().density;
            scale = scalePercent / 100f;
            width = px(198);
            height = px(174);
            setBackgroundColor(Color.TRANSPARENT);
            setWillNotDraw(false);

            backgroundPaint.setColor(0xe6111822);
            backgroundPaint.setStyle(Paint.Style.FILL);
            borderPaint.setColor(0x667f8fa3);
            borderPaint.setStyle(Paint.Style.STROKE);
            borderPaint.setStrokeWidth(Math.max(1f, px(1)));
            titlePaint.setColor(0xffe9f2fb);
            titlePaint.setTextSize(px(10));
            titlePaint.setTypeface(
                android.graphics.Typeface.DEFAULT_BOLD
            );
            textPaint.setColor(0xffb8c5d4);
            textPaint.setTextSize(px(8));
            barPaint.setStyle(Paint.Style.FILL);
            for (int index = 0; index < icons.length; ++index) {
                Drawable drawable = context.getDrawable(ICONS[index]);
                icons[index] = drawable == null ? null : drawable.mutate();
                items[index] = EquipmentItem.empty(index);
            }
        }

        int preferredWidth() { return width; }
        int preferredHeight() { return height; }

        void setItems(EquipmentItem[] value) {
            if (value != null && value.length == items.length) items = value;
            postInvalidateOnAnimation();
        }

        @Override
        protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            rectangle.set(0, 0, getWidth(), getHeight());
            canvas.drawRoundRect(rectangle, px(13), px(13), backgroundPaint);
            rectangle.inset(px(0.7f), px(0.7f));
            canvas.drawRoundRect(rectangle, px(13), px(13), borderPaint);
            titlePaint.setTextSize(px(10));
            canvas.drawText("СНАРЯЖЕНИЕ", px(10), px(16), titlePaint);

            boolean animate = false;
            for (int index = 0; index < items.length; ++index) {
                EquipmentItem item = items[index];
                float top = px(23 + index * 29);
                drawRow(canvas, item, top);
                animate |= item.enchanted && item.present;
            }
            if (animate) postInvalidateOnAnimation();
        }

        private void drawRow(Canvas canvas, EquipmentItem item, float top) {
            int iconSize = px(24);
            rectangle.set(px(8), top, px(8) + iconSize, top + iconSize);
            barPaint.setColor(item.present ? 0xff202c3a : 0xff19212c);
            canvas.drawRoundRect(rectangle, px(6), px(6), barPaint);

            Drawable icon = icons[item.slot];
            if (icon != null) {
                int color = item.present ? materialColor(item.name) : 0xff526071;
                if (item.present && item.enchanted) {
                    float phase = (SystemClock.uptimeMillis() % 1800L) / 1800f;
                    float wave = 0.5f + 0.5f * (float) Math.sin(
                        phase * Math.PI * 2.0 + item.slot * 0.8
                    );
                    color = blend(color, 0xffc26cff, 0.28f + wave * 0.34f);
                }
                icon.setTint(color);
                icon.setBounds(
                    Math.round(rectangle.left + px(4)),
                    Math.round(rectangle.top + px(4)),
                    Math.round(rectangle.right - px(4)),
                    Math.round(rectangle.bottom - px(4))
                );
                icon.draw(canvas);
            }

            float textX = px(38);
            titlePaint.setTextSize(px(8.5f));
            canvas.drawText(SLOT_NAMES[item.slot], textX, top + px(9), titlePaint);
            String itemName = item.present ? shortName(item.name) : "пусто";
            if (item.present && item.enchanted) itemName = "✦ " + itemName;
            textPaint.setColor(
                item.present && item.enchanted ? 0xffd9a2ff : 0xffb8c5d4
            );
            canvas.drawText(itemName, textX, top + px(20), textPaint);
            textPaint.setColor(0xffb8c5d4);

            int maximum = maximumDurability(item.name, item.slot);
            float barLeft = px(132);
            float barTop = top + px(6);
            float barWidth = px(58);
            rectangle.set(barLeft, barTop, barLeft + barWidth, barTop + px(5));
            barPaint.setColor(0xff344151);
            canvas.drawRoundRect(rectangle, px(3), px(3), barPaint);
            if (item.present && maximum > 0) {
                int remaining = Math.max(0, maximum - item.damage);
                float ratio = Math.max(0f, Math.min(1f, remaining / (float) maximum));
                rectangle.right = rectangle.left + barWidth * ratio;
                barPaint.setColor(durabilityColor(ratio));
                canvas.drawRoundRect(rectangle, px(3), px(3), barPaint);
                String value = String.format(
                    Locale.getDefault(),
                    "%d/%d",
                    remaining,
                    maximum
                );
                canvas.drawText(value, barLeft, top + px(21), textPaint);
            } else {
                canvas.drawText("—", barLeft, top + px(21), textPaint);
            }
        }

        private int px(float value) {
            return Math.max(1, Math.round(value * density * scale));
        }

        private static String shortName(String name) {
            if (name == null || name.isEmpty()) return "предмет";
            int separator = name.indexOf(':');
            String result = separator >= 0 ? name.substring(separator + 1) : name;
            result = result.replace('_', ' ');
            if (result.length() > 14) result = result.substring(0, 14) + "…";
            return result;
        }

        private static int materialColor(String name) {
            String value = name == null ? "" : name;
            if (value.contains("netherite")) return 0xff8b758f;
            if (value.contains("diamond")) return 0xff57e3df;
            if (value.contains("gold")) return 0xffffd45d;
            if (value.contains("iron")) return 0xffdbe3e9;
            if (value.contains("chainmail")) return 0xffa9b6c2;
            if (value.contains("leather")) return 0xffb77a4d;
            if (value.contains("turtle")) return 0xff55bd77;
            return 0xffd9e7f5;
        }

        private static int durabilityColor(float ratio) {
            if (ratio > 0.55f) return 0xff68d98b;
            if (ratio > 0.25f) return 0xffffc857;
            return 0xffff6575;
        }

        private static int maximumDurability(String name, int slot) {
            if (name == null || name.isEmpty()) return 0;
            if (slot == 0) return handDurability(name);
            int material;
            if (name.contains("netherite")) material = 5;
            else if (name.contains("diamond")) material = 4;
            else if (name.contains("iron") || name.contains("chainmail")) {
                material = 2;
            } else if (name.contains("gold")) material = 3;
            else if (name.contains("leather")) material = 1;
            else if (name.contains("turtle") && slot == 1) return 275;
            else return 0;
            int[][] durability = {
                {0, 0, 0, 0},
                {55, 80, 75, 65},
                {165, 240, 225, 195},
                {77, 112, 105, 91},
                {363, 528, 495, 429},
                {407, 592, 555, 481}
            };
            return durability[material][slot - 1];
        }

        private static int handDurability(String name) {
            if (name.contains("shield")) return 336;
            if (name.contains("crossbow")) return 465;
            if (name.endsWith(":bow") || name.equals("bow")) return 384;
            if (name.contains("trident")) return 250;
            if (name.contains("fishing_rod")) return 64;
            if (name.contains("flint_and_steel")) return 64;
            if (name.contains("shears")) return 238;
            if (name.contains("elytra")) return 432;
            if (name.contains("carrot_on_a_stick")) return 25;
            if (name.contains("warped_fungus_on_a_stick")) return 100;
            if (name.contains("brush")) return 64;
            if (name.contains("mace")) return 500;
            boolean materialTool = name.contains("sword") ||
                name.contains("pickaxe") || name.contains("shovel") ||
                name.contains("_axe") || name.contains("_hoe");
            if (!materialTool) return 0;
            int base;
            if (name.contains("netherite")) base = 2031;
            else if (name.contains("diamond")) base = 1561;
            else if (name.contains("iron")) base = 250;
            else if (name.contains("stone")) base = 131;
            else if (name.contains("gold")) base = 32;
            else if (name.contains("wooden")) base = 59;
            else return 0;
            return base;
        }

        private static int blend(int first, int second, float amount) {
            float value = Math.max(0f, Math.min(1f, amount));
            return Color.rgb(
                Math.round(Color.red(first) * (1f - value) +
                    Color.red(second) * value),
                Math.round(Color.green(first) * (1f - value) +
                    Color.green(second) * value),
                Math.round(Color.blue(first) * (1f - value) +
                    Color.blue(second) * value)
            );
        }
    }
}
