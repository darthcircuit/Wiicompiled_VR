package org.wiicompiled.quest.launcher

import android.app.Activity
import android.app.AlertDialog
import android.content.res.ColorStateList
import android.net.Uri
import android.provider.OpenableColumns
import android.text.TextUtils
import android.util.Log
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.view.WindowManager
import android.view.inputmethod.EditorInfo
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.PopupMenu
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast
import java.io.File
import java.io.IOException
import kotlin.math.roundToInt
import org.wiicompiled.quest.GameStorage
import org.wiicompiled.quest.R

/**
 * The launcher's Patches page, WheelWizard's mods page (Views/Pages/ModsPage.axaml.cs) without its
 * mod browser: Import turns picked files into a named mod, and each mod can be switched on or off,
 * moved up or down the list, renamed or deleted. What the list means for the game happens at
 * Play, in [ModLibrary.prepareForLaunch].
 */
class PatchesPage(
    private val activity: Activity,
    root: View,
    private val pickFiles: () -> Unit,
) {

    private val empty: View = root.findViewById(R.id.patches_empty)
    private val content: View = root.findViewById(R.id.patches_content)
    private val rows: LinearLayout = root.findViewById(R.id.patches_rows)
    private val count: TextView = root.findViewById(R.id.patches_count)
    private val enableAll: Switch = root.findViewById(R.id.patches_enable_all)
    private val headerImport: TextView = root.findViewById(R.id.patches_import)
    private val importButtons = listOf(headerImport, root.findViewById<TextView>(R.id.patches_empty_import))

    private val modsDir: File get() = GameStorage.modsDirectory(activity)

    /** The list as read for the rows on screen, top first. */
    private var mods: List<ModLibrary.Mod> = emptyList()

    init {
        for (button in importButtons) {
            button.setOnClickListener { if (!ModLibrary.importing) pickFiles() }
        }
        styleSwitch(enableAll)
        // A click, not a checked change: refresh() sets the switch without meaning to change every mod.
        enableAll.setOnClickListener { setAllEnabled(enableAll.isChecked) }
        root.findViewById<View>(R.id.patches_enable_all_label).setOnClickListener {
            enableAll.toggle()
            setAllEnabled(enableAll.isChecked)
        }
    }

    /** Rebuilds the list from the Mods folder, which adb or the PC launcher's files may have changed. */
    fun refresh() {
        mods = ModLibrary.load(modsDir)
        val hasMods = mods.isNotEmpty()
        // As on the PC, the page's own Import moves to the top bar once there is a list.
        empty.visibility = if (hasMods) View.GONE else View.VISIBLE
        content.visibility = if (hasMods) View.VISIBLE else View.GONE
        headerImport.visibility = if (hasMods) View.VISIBLE else View.GONE
        count.text = mods.size.toString()
        enableAll.isChecked = mods.all { it.enabled }

        val importing = ModLibrary.importing
        for (button in importButtons) {
            button.isEnabled = !importing
            button.alpha = if (importing) 0.45f else 1f
            button.setText(if (importing) R.string.patches_importing else R.string.patches_import)
        }

        rows.removeAllViews()
        if (!hasMods) return
        rows.addView(
            note(),
            LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                bottomMargin = dp(12)
            },
        )
        mods.forEachIndexed { index, mod ->
            rows.addView(
                row(mod, index),
                LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                    if (index > 0) topMargin = dp(3)
                },
            )
        }
    }

    /** The files the player picked for Import: asks for the mod's name, then copies them in the background. */
    fun importPicked(uris: List<Uri>) {
        if (uris.isEmpty() || ModLibrary.importing) return
        mods = ModLibrary.load(modsDir)
        val sources = uris.map { uri ->
            ModLibrary.Source(displayName(uri)) {
                activity.contentResolver.openInputStream(uri) ?: throw IOException("Cannot read $uri")
            }
        }
        // Typing on a headset is slow, so a single file offers its own name to start from.
        val suggested = if (sources.size == 1) ModLibrary.suggestName(sources[0].name, mods) else ""
        nameDialog(R.string.patches_name_title, null, suggested, R.string.patches_import, { ModLibrary.validateName(it, mods) }) { name ->
            Log.i(TAG, "Importing ${sources.size} file(s) as mod $name")
            ModLibrary.importInBackground(modsDir, name, sources) { result ->
                if (activity.isDestroyed) return@importInBackground
                refresh()
                result.onSuccess { mod ->
                    Toast.makeText(activity, activity.getString(R.string.patches_installed, mod.title), Toast.LENGTH_LONG).show()
                }.onFailure { failure ->
                    Log.w(TAG, "Mod import failed", failure)
                    AlertDialog.Builder(activity)
                        .setTitle(R.string.patches_import_failed)
                        .setMessage(failure.message ?: failure.toString())
                        .setPositiveButton(android.R.string.ok, null)
                        .show()
                }
            }
            refresh()
        }
    }

    private fun row(mod: ModLibrary.Mod, index: Int): View {
        val last = mods.size - 1
        val toggle = Switch(activity).apply {
            isChecked = mod.enabled
            styleSwitch(this)
            contentDescription = mod.title
            setOnCheckedChangeListener { _, checked -> setModEnabled(mod, checked) }
        }
        val title = TextView(activity).apply {
            text = mod.title
            setTextColor(activity.getColor(R.color.neutral_100))
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f)
            isSingleLine = true
            ellipsize = TextUtils.TruncateAt.END
        }
        return LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            minimumHeight = dp(58)
            setPadding(dp(14), dp(6), dp(8), dp(6))
            background = activity.getDrawable(
                when {
                    last == 0 -> R.drawable.bg_row_single
                    index == 0 -> R.drawable.bg_row_top
                    index == last -> R.drawable.bg_row_bottom
                    else -> R.drawable.bg_row_middle
                },
            )
            addView(toggle)
            addView(title, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f).apply {
                marginStart = dp(14)
                marginEnd = dp(8)
            })
            addView(iconButton(R.drawable.ic_chevron_up, R.string.patches_move_up, index > 0) { move(mod, up = true) })
            addView(iconButton(R.drawable.ic_chevron_down, R.string.patches_move_down, index < last) { move(mod, up = false) })
            addView(iconButton(R.drawable.ic_more, R.string.patches_more, true) { anchor -> showMenu(anchor, mod) })
            setOnClickListener { toggle.toggle() }
        }
    }

    private fun iconButton(icon: Int, description: Int, enabled: Boolean, onClick: (View) -> Unit) = ImageView(activity).apply {
        setImageResource(icon)
        imageTintList = ColorStateList.valueOf(activity.getColor(R.color.neutral_300))
        background = activity.getDrawable(R.drawable.bg_nav_item)
        contentDescription = activity.getString(description)
        setPadding(dp(10), dp(10), dp(10), dp(10))
        isEnabled = enabled
        alpha = if (enabled) 1f else 0.3f
        setOnClickListener(onClick)
        layoutParams = LinearLayout.LayoutParams(dp(42), dp(42))
    }

    private fun note() = LinearLayout(activity).apply {
        orientation = LinearLayout.HORIZONTAL
        background = activity.getDrawable(R.drawable.bg_banner_info)
        setPadding(dp(14), dp(10), dp(14), dp(10))
        addView(
            TextView(activity).apply {
                setText(R.string.patches_note)
                setTextColor(activity.getColor(R.color.neutral_300))
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
            },
        )
    }

    private fun showMenu(anchor: View, mod: ModLibrary.Mod) {
        PopupMenu(activity, anchor).apply {
            menu.add(0, MENU_RENAME, 0, R.string.patches_rename)
            menu.add(0, MENU_DELETE, 1, R.string.patches_delete)
            setOnMenuItemClickListener { item ->
                when (item.itemId) {
                    MENU_RENAME -> rename(mod)
                    MENU_DELETE -> delete(mod)
                }
                true
            }
            show()
        }
    }

    private fun setModEnabled(mod: ModLibrary.Mod, enabled: Boolean) {
        // Saved in place rather than rebuilt, so the switch keeps its animation.
        val changed = mod.copy(enabled = enabled)
        if (!change { ModLibrary.save(modsDir, changed) }) {
            refresh()
            return
        }
        mods = mods.map { if (it.title == mod.title) changed else it }
        enableAll.isChecked = mods.all { it.enabled }
    }

    private fun setAllEnabled(enabled: Boolean) {
        change {
            for (mod in mods) {
                if (mod.enabled != enabled) ModLibrary.save(modsDir, mod.copy(enabled = enabled))
            }
        }
        refresh()
    }

    private fun move(mod: ModLibrary.Mod, up: Boolean) {
        change { ModLibrary.move(mods, mod, up).forEach { ModLibrary.save(modsDir, it) } }
        refresh()
    }

    private fun rename(mod: ModLibrary.Mod) {
        nameDialog(
            R.string.patches_rename_title,
            activity.getString(R.string.patches_rename_message, mod.title),
            mod.title,
            R.string.patches_rename,
            { name -> if (name.trim() == mod.title) null else ModLibrary.validateName(name, mods) },
        ) { name ->
            change { ModLibrary.rename(modsDir, mod, name, mods) }
            refresh()
        }
    }

    private fun delete(mod: ModLibrary.Mod) {
        AlertDialog.Builder(activity)
            .setTitle(activity.getString(R.string.patches_delete_title, mod.title))
            .setMessage(R.string.patches_delete_message)
            .setPositiveButton(R.string.patches_delete) { _, _ ->
                change { ModLibrary.delete(modsDir, mod) }
                refresh()
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    /** Runs one change to the Mods folder; false, with the reason shown, when it failed. */
    private fun change(edit: () -> Unit): Boolean = try {
        edit()
        true
    } catch (e: IOException) {
        Log.w(TAG, "Mod change failed", e)
        Toast.makeText(activity, activity.getString(R.string.patches_change_failed, e.message ?: e.toString()), Toast.LENGTH_LONG).show()
        false
    }

    /**
     * WheelWizard's TextInputWindow: a name field that keeps the dialog open, with the reason shown,
     * until the name is one [validate] accepts.
     */
    private fun nameDialog(
        title: Int,
        message: String?,
        initial: String,
        positive: Int,
        validate: (String) -> ModLibrary.NameProblem?,
        onAccept: (String) -> Unit,
    ) {
        val input = EditText(activity).apply {
            setText(initial)
            setSelection(text.length)
            setHint(R.string.patches_name_hint)
            setTextColor(activity.getColor(R.color.neutral_100))
            setHintTextColor(activity.getColor(R.color.neutral_500))
            isSingleLine = true
            imeOptions = EditorInfo.IME_ACTION_DONE
        }
        val container = FrameLayout(activity).apply {
            setPadding(dp(22), dp(8), dp(22), 0)
            addView(input)
        }
        val dialog = AlertDialog.Builder(activity)
            .setTitle(title)
            .apply { message?.let { setMessage(it) } }
            .setView(container)
            .setPositiveButton(positive, null)
            .setNegativeButton(android.R.string.cancel, null)
            .create()
        fun accept() {
            val problem = validate(input.text.toString())
            if (problem != null) {
                input.error = activity.getString(
                    when (problem) {
                        ModLibrary.NameProblem.Empty -> R.string.patches_name_empty
                        ModLibrary.NameProblem.Exists -> R.string.patches_name_exists
                        ModLibrary.NameProblem.IllegalCharacters -> R.string.patches_name_illegal
                    },
                )
                return
            }
            dialog.dismiss()
            onAccept(input.text.toString().trim())
        }
        input.setOnEditorActionListener { _, action, _ ->
            if (action == EditorInfo.IME_ACTION_DONE) accept()
            action == EditorInfo.IME_ACTION_DONE
        }
        dialog.setOnShowListener {
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener { accept() }
            input.requestFocus()
        }
        dialog.window?.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_VISIBLE)
        dialog.show()
    }

    private fun displayName(uri: Uri): String {
        runCatching {
            activity.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
                if (cursor.moveToFirst()) cursor.getString(0)?.takeIf { it.isNotBlank() }?.let { return it }
            }
        }
        return uri.lastPathSegment?.substringAfterLast('/')?.takeIf { it.isNotBlank() } ?: "file"
    }

    private fun styleSwitch(switch: Switch) {
        switch.thumbTintList = checkedColors(R.color.neutral_50, R.color.neutral_300)
        switch.trackTintList = checkedColors(R.color.primary_400, R.color.neutral_600)
    }

    private fun checkedColors(checked: Int, unchecked: Int) = ColorStateList(
        arrayOf(intArrayOf(android.R.attr.state_checked), intArrayOf()),
        intArrayOf(activity.getColor(checked), activity.getColor(unchecked)),
    )

    private fun dp(value: Int): Int = (value * activity.resources.displayMetrics.density).roundToInt()

    private companion object {
        const val TAG = "WiiCompiledLauncher"
        const val MENU_RENAME = 1
        const val MENU_DELETE = 2
    }
}
