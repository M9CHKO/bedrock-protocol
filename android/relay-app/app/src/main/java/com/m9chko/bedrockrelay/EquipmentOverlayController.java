package com.m9chko.bedrockrelay;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.drawable.Drawable;
import android.os.SystemClock;
import android.provider.Settings;
import android.util.LruCache;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewConfiguration;
import android.view.WindowManager;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.util.HashSet;
import java.util.Locale;
import java.util.Set;

/** Draggable, session-scoped HUD for the local player's equipment. */
final class EquipmentOverlayController {
    private final Context context;
    private final WindowManager windowManager;
    private final OfficialTexturePack texturePack;
    private final SharedPreferences texturePreferences;
    private final SharedPreferences.OnSharedPreferenceChangeListener
        texturePreferenceListener;
    private boolean sessionVisible;
    private boolean uiBlocked;
    private boolean enabled = true;
    private int scalePercent = 100;
    private EquipmentView view;
    private WindowManager.LayoutParams windowParams;
    private long lastRevision = Long.MIN_VALUE;
    private EquipmentItem[] latestItems;

    EquipmentOverlayController(Context context) {
        this.context = context;
        this.windowManager = (WindowManager) context.getSystemService(
            Context.WINDOW_SERVICE
        );
        this.texturePack = new OfficialTexturePack(context);
        this.texturePreferences = context.getSharedPreferences(
            RelayService.PREFERENCES,
            Context.MODE_PRIVATE
        );
        this.texturePreferenceListener = (preferences, key) -> {
            if (!OfficialTexturePack.KEY_REVISION.equals(key)) return;
            EquipmentView current = view;
            if (current != null) current.post(current::reloadTextures);
        };
        texturePreferences.registerOnSharedPreferenceChangeListener(
            texturePreferenceListener
        );
    }

    void setSessionVisible(boolean visible) {
        sessionVisible = visible;
        reconcile();
    }

    void setUiBlocked(boolean blocked) {
        if (uiBlocked == blocked) return;
        uiBlocked = blocked;
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
        EquipmentItem[] items = new EquipmentItem[6];
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

    void destroy() {
        hideImmediately();
        texturePreferences.unregisterOnSharedPreferenceChangeListener(
            texturePreferenceListener
        );
    }

    private void reconcile() {
        if (sessionVisible && enabled && !uiBlocked) addWindow();
        else removeWindow();
    }

    private void addWindow() {
        if (view != null || !Settings.canDrawOverlays(context)) return;
        EquipmentView added = new EquipmentView(
            context,
            scalePercent,
            texturePack
        );
        WindowManager.LayoutParams params = new WindowManager.LayoutParams(
            added.preferredWidth(),
            added.preferredHeight(),
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE |
                WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL |
                WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
            PixelFormat.TRANSLUCENT
        );
        params.gravity = Gravity.TOP | Gravity.START;
        int screenWidth = context.getResources()
            .getDisplayMetrics().widthPixels;
        int defaultX = Math.max(
            0,
            screenWidth - added.preferredWidth() - dp(12)
        );
        params.x = texturePreferences.getInt(
            RelayService.KEY_EQUIPMENT_HUD_X,
            defaultX
        );
        params.y = texturePreferences.getInt(
            RelayService.KEY_EQUIPMENT_HUD_Y,
            dp(46)
        );
        attachDrag(added, params);
        try {
            windowManager.addView(added, params);
            view = added;
            windowParams = params;
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
        windowParams = null;
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
            case "offhand": return 1;
            case "helmet": return 2;
            case "chestplate": return 3;
            case "leggings": return 4;
            case "boots": return 5;
            default: return -1;
        }
    }

    private void attachDrag(
        EquipmentView target,
        WindowManager.LayoutParams targetParams
    ) {
        final int slop = ViewConfiguration.get(context)
            .getScaledTouchSlop();
        target.setOnTouchListener(new View.OnTouchListener() {
            private float downRawX;
            private float downRawY;
            private int downX;
            private int downY;
            private boolean moved;

            @Override
            public boolean onTouch(View touched, MotionEvent event) {
                if (view != target || windowParams != targetParams) {
                    return false;
                }
                switch (event.getActionMasked()) {
                    case MotionEvent.ACTION_DOWN:
                        downRawX = event.getRawX();
                        downRawY = event.getRawY();
                        downX = targetParams.x;
                        downY = targetParams.y;
                        moved = false;
                        return true;
                    case MotionEvent.ACTION_MOVE:
                        float dx = event.getRawX() - downRawX;
                        float dy = event.getRawY() - downRawY;
                        moved |= Math.abs(dx) > slop || Math.abs(dy) > slop;
                        if (!moved) return true;
                        int screenWidth = context.getResources()
                            .getDisplayMetrics().widthPixels;
                        int screenHeight = context.getResources()
                            .getDisplayMetrics().heightPixels;
                        targetParams.x = Math.max(
                            0,
                            Math.min(
                                Math.max(0, screenWidth - target.getWidth()),
                                downX + Math.round(dx)
                            )
                        );
                        targetParams.y = Math.max(
                            0,
                            Math.min(
                                Math.max(0, screenHeight - target.getHeight()),
                                downY + Math.round(dy)
                            )
                        );
                        try {
                            windowManager.updateViewLayout(target, targetParams);
                        } catch (Throwable ignored) {
                        }
                        return true;
                    case MotionEvent.ACTION_UP:
                    case MotionEvent.ACTION_CANCEL:
                        if (moved) {
                            texturePreferences.edit()
                                .putInt(
                                    RelayService.KEY_EQUIPMENT_HUD_X,
                                    targetParams.x
                                )
                                .putInt(
                                    RelayService.KEY_EQUIPMENT_HUD_Y,
                                    targetParams.y
                                )
                                .apply();
                        }
                        return true;
                    default:
                        return false;
                }
            }
        });
    }

    private static final class EquipmentItem {
        final int slot;
        final boolean present;
        final String name;
        final int damage;
        final boolean durabilityKnown;
        final int maximumDurability;
        final int remainingDurability;
        final int durabilityPercent;
        final boolean enchanted;

        EquipmentItem(
            int slot,
            boolean present,
            String name,
            int damage,
            boolean damageKnown,
            int maximumDurability,
            int remainingDurability,
            int durabilityPercent,
            boolean enchanted
        ) {
            this.slot = slot;
            this.present = present;
            this.name = name;
            this.damage = Math.max(0, damage);
            this.maximumDurability = Math.max(0, maximumDurability);
            this.remainingDurability = Math.max(
                0,
                Math.min(this.maximumDurability, remainingDurability)
            );
            this.durabilityPercent = Math.max(
                0,
                Math.min(100, durabilityPercent)
            );
            this.durabilityKnown = damageKnown && this.maximumDurability > 0;
            this.enchanted = enchanted;
        }

        static EquipmentItem empty(int slot) {
            return new EquipmentItem(
                slot,
                false,
                "",
                0,
                false,
                0,
                0,
                0,
                false
            );
        }

        static EquipmentItem from(int slot, JSONObject value) {
            return new EquipmentItem(
                slot,
                value.optBoolean("present", false),
                value.optString("name", ""),
                value.optInt("damage", 0),
                value.optBoolean("damageKnown", false),
                value.optInt("maximumDurability", 0),
                value.optInt("remainingDurability", 0),
                value.optInt("durabilityPercent", 0),
                value.optBoolean("enchanted", false)
            );
        }
    }

    private static final class EquipmentView extends View {
        private static final String[] SLOT_NAMES = {
            "ПРАВАЯ РУКА", "ЛЕВАЯ РУКА", "ШЛЕМ", "НАГРУДНИК",
            "ПОНОЖИ", "БОТИНКИ"
        };
        private static final int[] ICONS = {
            R.drawable.ic_equipment_hand,
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
        private final Paint bitmapPaint = new Paint();
        private final Paint glintPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF rectangle = new RectF();
        private final RectF bitmapDestination = new RectF();
        private final RectF glintDestination = new RectF();
        private final Rect bitmapSource = new Rect();
        private final Drawable[] icons = new Drawable[6];
        private final OfficialTexturePack texturePack;
        private final Set<String> missingTextures = new HashSet<>();
        private final LruCache<String, Bitmap> textureCache =
            new LruCache<String, Bitmap>(6 * 1024 * 1024) {
                @Override
                protected int sizeOf(String key, Bitmap value) {
                    return value.getAllocationByteCount();
                }
            };
        private long textureRevision = Long.MIN_VALUE;
        private Bitmap enchantmentGlint;
        private EquipmentItem[] items = new EquipmentItem[6];

        EquipmentView(
            Context context,
            int scalePercent,
            OfficialTexturePack texturePack
        ) {
            super(context);
            this.texturePack = texturePack;
            density = context.getResources().getDisplayMetrics().density;
            scale = scalePercent / 100f;
            width = px(198);
            height = px(203);
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
            bitmapPaint.setAntiAlias(false);
            bitmapPaint.setFilterBitmap(false);
            bitmapPaint.setDither(false);
            glintPaint.setStyle(Paint.Style.STROKE);
            glintPaint.setStrokeWidth(Math.max(1f, px(2)));
            for (int index = 0; index < icons.length; ++index) {
                Drawable drawable = context.getDrawable(ICONS[index]);
                icons[index] = drawable == null ? null : drawable.mutate();
                items[index] = EquipmentItem.empty(index);
            }
            reloadTextures();
        }

        int preferredWidth() { return width; }
        int preferredHeight() { return height; }

        void setItems(EquipmentItem[] value) {
            if (value != null && value.length == items.length) items = value;
            postInvalidateOnAnimation();
        }

        void reloadTextures() {
            textureRevision = texturePack.revision();
            textureCache.evictAll();
            missingTextures.clear();
            enchantmentGlint = decode(texturePack.enchantmentGlintFile());
            postInvalidateOnAnimation();
        }

        @Override
        protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            if (textureRevision != texturePack.revision()) reloadTextures();
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

            bitmapDestination.set(
                rectangle.left + px(2),
                rectangle.top + px(2),
                rectangle.right - px(2),
                rectangle.bottom - px(2)
            );
            Bitmap official = item.present ? bitmapFor(item.name) : null;
            if (official != null) {
                bitmapSource.set(0, 0, official.getWidth(), official.getHeight());
                bitmapPaint.setAlpha(255);
                canvas.drawBitmap(
                    official,
                    bitmapSource,
                    bitmapDestination,
                    bitmapPaint
                );
            } else {
                Drawable icon = icons[item.slot];
                if (icon != null) {
                    int color = item.present
                        ? materialColor(item.name)
                        : 0xff526071;
                    if (item.present && item.enchanted) {
                        float phase = (SystemClock.uptimeMillis() % 1800L) /
                            1800f;
                        float wave = 0.5f + 0.5f * (float) Math.sin(
                            phase * Math.PI * 2.0 + item.slot * 0.8
                        );
                        color = blend(
                            color,
                            0xffc26cff,
                            0.28f + wave * 0.34f
                        );
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
            }
            if (item.present && item.enchanted) {
                drawEnchantmentGlint(canvas, bitmapDestination, item.slot);
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

            float barLeft = px(132);
            float barTop = top + px(6);
            float barWidth = px(58);
            rectangle.set(barLeft, barTop, barLeft + barWidth, barTop + px(5));
            barPaint.setColor(0xff344151);
            canvas.drawRoundRect(rectangle, px(3), px(3), barPaint);
            if (item.present && item.durabilityKnown) {
                float ratio = item.remainingDurability /
                    (float) item.maximumDurability;
                rectangle.right = rectangle.left + barWidth * ratio;
                barPaint.setColor(durabilityColor(ratio));
                canvas.drawRoundRect(rectangle, px(3), px(3), barPaint);
                String value = String.format(
                    Locale.getDefault(),
                    "%d/%d",
                    item.remainingDurability,
                    item.maximumDurability
                );
                canvas.drawText(value, barLeft, top + px(21), textPaint);
            } else {
                canvas.drawText("—", barLeft, top + px(21), textPaint);
            }
        }

        private Bitmap bitmapFor(String registryName) {
            String key = registryName == null ? "" : registryName;
            Bitmap cached = textureCache.get(key);
            if (cached != null) return cached;
            if (missingTextures.contains(key)) return null;
            Bitmap loaded = decode(texturePack.textureFile(key));
            if (loaded == null) {
                missingTextures.add(key);
                return null;
            }
            textureCache.put(key, loaded);
            return loaded;
        }

        private static Bitmap decode(File value) {
            if (value == null || !value.isFile()) return null;
            BitmapFactory.Options options = new BitmapFactory.Options();
            options.inScaled = false;
            options.inPreferredConfig = Bitmap.Config.ARGB_8888;
            try {
                return BitmapFactory.decodeFile(value.getAbsolutePath(), options);
            } catch (Throwable ignored) {
                return null;
            }
        }

        private void drawEnchantmentGlint(
            Canvas canvas,
            RectF bounds,
            int slot
        ) {
            int saved = canvas.save();
            canvas.clipRect(bounds);
            float phase = (SystemClock.uptimeMillis() % 2200L) / 2200f;
            canvas.rotate(-28f, bounds.centerX(), bounds.centerY());
            if (enchantmentGlint != null) {
                bitmapSource.set(
                    0,
                    0,
                    enchantmentGlint.getWidth(),
                    enchantmentGlint.getHeight()
                );
                float tile = Math.max(bounds.width(), bounds.height()) * 2.35f;
                float left = bounds.left - tile + phase * tile * 2f;
                bitmapPaint.setAlpha(92);
                for (int index = -1; index <= 1; ++index) {
                    float x = left + index * tile;
                    glintDestination.set(
                        x,
                        bounds.centerY() - tile * 0.5f,
                        x + tile,
                        bounds.centerY() + tile * 0.5f
                    );
                    canvas.drawBitmap(
                        enchantmentGlint,
                        bitmapSource,
                        glintDestination,
                        bitmapPaint
                    );
                }
                bitmapPaint.setAlpha(255);
            } else {
                glintPaint.setColor(0x88cf76ff);
                float spacing = px(8);
                float offset = (phase * spacing * 2f + slot * px(2)) % spacing;
                for (float x = bounds.left - bounds.height();
                     x < bounds.right + bounds.height();
                     x += spacing) {
                    canvas.drawLine(
                        x + offset,
                        bounds.bottom + px(4),
                        x + offset + bounds.height(),
                        bounds.top - px(4),
                        glintPaint
                    );
                }
            }
            canvas.restoreToCount(saved);
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
