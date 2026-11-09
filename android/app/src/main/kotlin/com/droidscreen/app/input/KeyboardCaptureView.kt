package com.droidscreen.app.input

import android.content.Context
import android.text.InputType
import android.util.AttributeSet
import android.view.KeyCharacterMap
import android.view.KeyEvent
import android.view.View
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import java.text.BreakIterator

/**
 * Invisible view that receives the soft keyboard and forwards what is typed to
 * the Mac as text or special keys. Nothing is kept on the tablet side.
 *
 * Keyboards may compose (setComposingText) before committing; the composing
 * text is typed on the Mac as it changes, and corrected with backspaces when
 * the keyboard replaces it, so the Mac always shows what the keyboard shows.
 */
class KeyboardCaptureView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : View(context, attrs) {

    /** UTF-8 text to type on the Mac. */
    var onText: ((String) -> Unit)? = null
    /** One special key (KeyCodes.*). */
    var onKey: ((Int) -> Unit)? = null

    /**
     * What we typed on the Mac since the last reset, split like an editor:
     * `committed` text, then the `composing` text the keyboard may still
     * replace. Deletions and replacements are applied to this model and the
     * difference is sent as backspaces + text, so the Mac matches the keyboard.
     */
    private val committed = StringBuilder()
    private var composing = ""
    /** Bumped per input session; calls from an older connection are ignored. */
    private var connectionGen = 0
    /** Pending hardware dead key (accent), combined with the next character. */
    private var pendingDeadChar = 0

    /** Called when the keyboard is dismissed with Back (all Android versions). */
    var onImeDismissed: (() -> Unit)? = null

    init {
        isFocusable = true
        isFocusableInTouchMode = true
    }

    override fun onCheckIsTextEditor(): Boolean = true

    override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection {
        // Visible-password + no suggestions: keyboards commit characters
        // directly instead of composing words and autocorrecting.
        outAttrs.inputType = InputType.TYPE_CLASS_TEXT or
            InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD or
            InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
        outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI or
            EditorInfo.IME_FLAG_NO_FULLSCREEN or
            EditorInfo.IME_ACTION_NONE
        resetComposition()
        return Connection(++connectionGen)
    }

    override fun onKeyPreIme(keyCode: Int, event: KeyEvent): Boolean {
        if (keyCode == KeyEvent.KEYCODE_BACK && event.action == KeyEvent.ACTION_UP) {
            onImeDismissed?.invoke()
        }
        return super.onKeyPreIme(keyCode, event)
    }

    /** Hardware keyboard keys, and keys some IMEs deliver as key events. */
    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean =
        handleKeyDown(event) || super.onKeyDown(keyCode, event)

    /** Forget the model (cursor moved, keyboard reopened): start fresh. */
    fun resetComposition() {
        committed.setLength(0)
        composing = ""
        pendingDeadChar = 0
    }

    private fun handleKeyDown(event: KeyEvent): Boolean {
        val special = when (event.keyCode) {
            KeyEvent.KEYCODE_DEL -> KeyCodes.BACKSPACE
            KeyEvent.KEYCODE_FORWARD_DEL -> KeyCodes.FORWARD_DEL
            KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_NUMPAD_ENTER -> KeyCodes.ENTER
            KeyEvent.KEYCODE_TAB -> KeyCodes.TAB
            KeyEvent.KEYCODE_ESCAPE -> KeyCodes.ESCAPE
            KeyEvent.KEYCODE_DPAD_LEFT -> KeyCodes.LEFT
            KeyEvent.KEYCODE_DPAD_RIGHT -> KeyCodes.RIGHT
            KeyEvent.KEYCODE_DPAD_UP -> KeyCodes.UP
            KeyEvent.KEYCODE_DPAD_DOWN -> KeyCodes.DOWN
            KeyEvent.KEYCODE_MOVE_HOME -> KeyCodes.HOME
            KeyEvent.KEYCODE_MOVE_END -> KeyCodes.END
            else -> 0
        }
        if (special != 0) {
            pendingDeadChar = 0
            if (special == KeyCodes.BACKSPACE) {
                dropLastGrapheme()
            } else {
                // The cursor moves or leaves the line: the model no longer applies.
                resetComposition()
            }
            onKey?.invoke(special)
            return true
        }
        val c = event.getUnicodeChar(event.metaState)
        if (c == 0) return false
        if ((c and KeyCharacterMap.COMBINING_ACCENT) != 0) {
            val accent = c and KeyCharacterMap.COMBINING_ACCENT_MASK
            if (pendingDeadChar != 0) {
                // Two dead keys: same accent twice -> its spacing form, else
                // type the first one's spacing form and keep the new one pending.
                val pending = pendingDeadChar
                pendingDeadChar = 0
                val combined = KeyCharacterMap.getDeadChar(pending, accent)
                if (combined != 0) { typeText(String(Character.toChars(combined))); return true }
                typeSpacingAccent(pending)
            }
            pendingDeadChar = accent
            return true
        }
        if (pendingDeadChar != 0) {
            val pending = pendingDeadChar
            pendingDeadChar = 0
            val combined = KeyCharacterMap.getDeadChar(pending, c)
            if (combined != 0) {
                typeText(String(Character.toChars(combined)))
                return true
            }
            // Not combinable (e.g. accent + q): keep the accent, then the character.
            typeSpacingAccent(pending)
        }
        typeText(String(Character.toChars(c)))
        return true
    }

    private fun typeSpacingAccent(accent: Int) {
        val spacing = KeyCharacterMap.getDeadChar(accent, ' '.code)
        if (spacing != 0) typeText(String(Character.toChars(spacing)))
    }

    /** Type committed text (hardware keys): ends any composition first. */
    private fun typeText(text: String) {
        finishComposing()
        sendText(text)
        committed.append(text)
        trimCommitted()
    }

    /** Type `text`, turning newlines into Enter (Unicode "\n" isn't Return on the Mac). */
    private fun sendText(text: String) {
        if (text.isEmpty()) return
        val parts = text.split('\n')
        parts.forEachIndexed { i, part ->
            if (part.isNotEmpty()) onText?.invoke(part)
            if (i < parts.size - 1) onKey?.invoke(KeyCodes.ENTER)
        }
    }

    /** Make the Mac show `next` where it now shows `prev` (both end at the cursor). */
    private fun applyEdit(prev: String, next: String) {
        val common = graphemeCommonPrefix(prev, next)
        repeat(graphemeCount(prev.substring(common))) { onKey?.invoke(KeyCodes.BACKSPACE) }
        sendText(next.substring(common))
    }

    private fun finishComposing() {
        committed.append(composing)
        composing = ""
        trimCommitted()
    }

    /**
     * Keep only the recent committed text, so edits stay cheap after long
     * typing or a big paste. Older text is treated as unknown (one Backspace
     * per unit if a keyboard ever deletes that far back).
     */
    private fun trimCommitted() {
        if (committed.length <= MAX_KNOWN * 2) return
        val s = committed.toString()
        val it = BreakIterator.getCharacterInstance().apply { setText(s) }
        val target = it.following(s.length - MAX_KNOWN).let { if (it == BreakIterator.DONE) s.length else it }
        // Always keep at least the last grapheme, even if it alone is long.
        val cut = minOf(target, lastGraphemeStart(s))
        committed.delete(0, cut)
    }

    /** A Backspace key was pressed: the Mac deletes one character; mirror it. */
    private fun dropLastGrapheme() {
        // Boundaries come from the whole text: a grapheme can span both parts.
        val full = committed.toString() + composing
        if (full.isEmpty()) return
        setModel(full.substring(0, lastGraphemeStart(full)))
    }

    /** Store `full` (text before the cursor), keeping as much composing text as survives. */
    private fun setModel(full: String) {
        if (full.length >= committed.length && full.startsWith(committed)) {
            composing = full.substring(committed.length)
        } else {
            committed.setLength(0)
            committed.append(full)
            composing = ""
        }
    }

    /**
     * Delete `before` UTF-16 units before the cursor (the composing text is
     * shortened rather than dropped, per the InputConnection contract).
     * Units beyond what we typed are unknown text on the Mac: one Backspace each.
     */
    private fun deleteBefore(before: Int, after: Int) {
        val full = committed.toString() + composing
        val known = minOf(before, full.length)
        var cut = full.length - known
        if (cut > 0 && cut < full.length && Character.isLowSurrogate(full[cut])) cut--
        val next = full.substring(0, cut)
        applyEdit(full, next)
        setModel(next)
        repeat((before - known).coerceIn(0, MAX_DELETE)) { onKey?.invoke(KeyCodes.BACKSPACE) }
        repeat(after.coerceIn(0, MAX_DELETE)) { onKey?.invoke(KeyCodes.FORWARD_DEL) }
    }

    /** UTF-16 length of the last `codePoints` code points of the known text (+ unknown extra). */
    private fun unitsForCodePoints(codePoints: Int): Int {
        val full = committed.toString() + composing
        var units = 0
        var cps = 0
        var i = full.length
        while (cps < codePoints && i > 0) {
            val cp = Character.codePointBefore(full, i)
            val n = Character.charCount(cp)
            i -= n
            units += n
            cps++
        }
        return units + (codePoints - cps)
    }

    private fun graphemeCount(s: String): Int {
        if (s.isEmpty()) return 0
        val it = BreakIterator.getCharacterInstance()
        it.setText(s)
        var n = 0
        while (it.next() != BreakIterator.DONE) n++
        return n
    }

    /** Longest common prefix that ends on a grapheme boundary in both strings. */
    private fun graphemeCommonPrefix(a: String, b: String): Int {
        if (a.isEmpty() || b.isEmpty()) return 0
        val ia = BreakIterator.getCharacterInstance().apply { setText(a) }
        val ib = BreakIterator.getCharacterInstance().apply { setText(b) }
        var prefix = 0
        var ea = ia.next()
        var eb = ib.next()
        while (ea != BreakIterator.DONE && eb != BreakIterator.DONE && ea == eb &&
            a.regionMatches(prefix, b, prefix, ea - prefix)) {
            prefix = ea
            ea = ia.next()
            eb = ib.next()
        }
        return prefix
    }

    private fun lastGraphemeStart(s: String): Int {
        if (s.isEmpty()) return 0
        val it = BreakIterator.getCharacterInstance()
        it.setText(s)
        val start = it.preceding(s.length)
        return if (start == BreakIterator.DONE) 0 else start
    }

    private inner class Connection(private val gen: Int) :
        BaseInputConnection(this@KeyboardCaptureView, false) {
        /** False once a newer session exists (e.g. after Back restarted input). */
        private val live: Boolean get() = gen == connectionGen

        // Diffs run on committed + composing so a grapheme spanning both
        // (a letter + a combining accent being composed) is replaced whole.
        override fun setComposingText(text: CharSequence, newCursorPosition: Int): Boolean {
            if (!live) return false
            val next = text.toString()
            val base = committed.toString()
            applyEdit(base + composing, base + next)
            composing = next
            return true
        }

        override fun commitText(text: CharSequence, newCursorPosition: Int): Boolean {
            if (!live) return false
            val next = text.toString()
            val base = committed.toString()
            applyEdit(base + composing, base + next)
            composing = ""
            committed.append(next)
            trimCommitted()
            return true
        }

        override fun finishComposingText(): Boolean {
            if (!live) return false
            finishComposing()
            return true
        }

        override fun deleteSurroundingText(beforeLength: Int, afterLength: Int): Boolean {
            if (!live) return false
            deleteBefore(beforeLength.coerceAtLeast(0), afterLength.coerceAtLeast(0))
            return true
        }

        override fun deleteSurroundingTextInCodePoints(beforeLength: Int, afterLength: Int): Boolean {
            if (!live) return false
            deleteBefore(unitsForCodePoints(beforeLength.coerceAtLeast(0)), afterLength.coerceAtLeast(0))
            return true
        }

        override fun sendKeyEvent(event: KeyEvent): Boolean {
            if (!live) return false
            if (event.action == KeyEvent.ACTION_DOWN) handleKeyDown(event)
            return true
        }

        override fun performEditorAction(actionCode: Int): Boolean {
            if (!live) return false
            resetComposition()
            onKey?.invoke(KeyCodes.ENTER)
            return true
        }

        // Surrounding text isn't reported: keyboards then send Backspace as
        // key events and don't try to edit text they can't see.
        override fun getTextBeforeCursor(n: Int, flags: Int): CharSequence = ""
        override fun getTextAfterCursor(n: Int, flags: Int): CharSequence = ""
    }

    private companion object {
        const val MAX_DELETE = 64
        /** Committed characters remembered for edits (trimmed past 2x this). */
        const val MAX_KNOWN = 64
    }
}

/** DS_KEY_* from protocol/include/droidscreen/keyboard.h. */
object KeyCodes {
    const val BACKSPACE = 1
    const val ENTER = 2
    const val TAB = 3
    const val ESCAPE = 4
    const val LEFT = 5
    const val RIGHT = 6
    const val UP = 7
    const val DOWN = 8
    const val FORWARD_DEL = 9
    const val HOME = 10
    const val END = 11
}
