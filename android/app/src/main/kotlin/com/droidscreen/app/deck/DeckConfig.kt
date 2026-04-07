package com.droidscreen.app.deck

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.util.Base64
import org.json.JSONObject

/**
 * Persistent configuration for the Stream Deck overlay grid.
 */
data class DeckConfig(
    val tiles: List<TileConfig>,
    val gridCols: Int,
    val gridRows: Int
) {

    companion object {

        fun createDefault(): DeckConfig = DeckConfig(
            tiles = listOf(
                TileConfig.AppTile(0, "app_safari", "Safari", 0, 0, 1, null),
                TileConfig.AppTile(1, "app_music", "Music", 0, 1, 1, null),
                TileConfig.AppTile(2, "app_notes", "Notes", 0, 2, 1, null),
                TileConfig.AppTile(3, "app_terminal", "Terminal", 0, 3, 1, null),
                TileConfig.MediaTile(4, "media", "Now Playing", 1, 0, colSpan = 4),
                TileConfig.VolumeTile(5, "volume", "Volume", 2, 0, colSpan = 4)
            ),
            gridCols = 4,
            gridRows = 3
        )

        fun fromJson(json: String): DeckConfig {
            val root = JSONObject(json)
            val gridCols = root.optInt("grid_cols", 3)
            val gridRows = root.optInt("grid_rows", 2)
            val tilesArray = root.getJSONArray("tiles")
            val tiles = mutableListOf<TileConfig>()

            for (i in 0 until tilesArray.length()) {
                val obj = tilesArray.getJSONObject(i)
                val id = obj.getString("id")
                val label = obj.optString("label", "")
                val row = obj.optInt("row", 0)
                val col = obj.optInt("col", 0)
                val colSpan = obj.optInt("col_span", 1)
                val type = obj.getString("type")

                val tile = when (type) {
                    "app" -> {
                        val iconBitmap = decodeBase64Bitmap(obj.optString("icon_b64", ""))
                        TileConfig.AppTile(i, id, label, row, col, colSpan, iconBitmap)
                    }
                    "media" -> TileConfig.MediaTile(i, id, label, row, col, colSpan)
                    "volume" -> TileConfig.VolumeTile(i, id, label, row, col, colSpan)
                    else -> TileConfig.MediaTile(i, id, label, row, col, colSpan)
                }
                tiles.add(tile)
            }

            return DeckConfig(tiles, gridCols, gridRows)
        }

        private fun decodeBase64Bitmap(encoded: String): Bitmap? {
            if (encoded.isBlank()) return null
            return try {
                val bytes = Base64.decode(encoded, Base64.DEFAULT)
                BitmapFactory.decodeByteArray(bytes, 0, bytes.size)
            } catch (_: Exception) {
                null
            }
        }
    }
}

/**
 * A single tile inside the deck grid. Subtypes carry type-specific payload.
 */
sealed class TileConfig(
    open val slotIndex: Int,
    open val id: String,
    open val label: String,
    open val row: Int,
    open val col: Int,
    open val colSpan: Int
) {

    data class AppTile(
        override val slotIndex: Int,
        override val id: String,
        override val label: String,
        override val row: Int,
        override val col: Int,
        override val colSpan: Int = 1,
        val iconBitmap: Bitmap?
    ) : TileConfig(slotIndex, id, label, row, col, colSpan)

    data class MediaTile(
        override val slotIndex: Int,
        override val id: String,
        override val label: String,
        override val row: Int,
        override val col: Int,
        override val colSpan: Int = 1
    ) : TileConfig(slotIndex, id, label, row, col, colSpan)

    data class VolumeTile(
        override val slotIndex: Int,
        override val id: String,
        override val label: String,
        override val row: Int,
        override val col: Int,
        override val colSpan: Int = 1
    ) : TileConfig(slotIndex, id, label, row, col, colSpan)
}

/**
 * Snapshot of the currently-playing media on the remote device.
 */
data class MediaState(
    val playing: Boolean,
    val title: String,
    val artist: String,
    val albumArtBitmap: Bitmap?,
    val progress: Float,
    val durationSec: Int
) {

    companion object {

        private var lastAlbumArtEncoded: String? = null
        private var lastAlbumArtBitmap: Bitmap? = null

        fun fromJson(json: String, previous: MediaState? = null): MediaState {
            val obj = JSONObject(json)
            val title = obj.optString("title", "")
            val artist = obj.optString("artist", "")
            val durationSec = obj.optInt("duration_sec", 0)
            val encodedAlbumArt = obj.optString("album_art_b64", "")
            val albumArt = when {
                encodedAlbumArt.isNotBlank() -> decodeBase64Bitmap(encodedAlbumArt)
                previous != null &&
                    previous.title == title &&
                    previous.artist == artist &&
                    previous.durationSec == durationSec -> previous.albumArtBitmap
                else -> null
            }
            return MediaState(
                playing = obj.optBoolean("playing", false),
                title = title,
                artist = artist,
                albumArtBitmap = albumArt,
                progress = obj.optDouble("progress", 0.0).toFloat(),
                durationSec = durationSec
            )
        }

        private fun decodeBase64Bitmap(encoded: String): Bitmap? {
            if (encoded.isBlank()) return null
            if (encoded == lastAlbumArtEncoded) {
                return lastAlbumArtBitmap
            }
            return try {
                val bytes = Base64.decode(encoded, Base64.DEFAULT)
                BitmapFactory.decodeByteArray(bytes, 0, bytes.size)?.also { bitmap ->
                    lastAlbumArtEncoded = encoded
                    lastAlbumArtBitmap = bitmap
                }
            } catch (_: Exception) {
                null
            }
        }
    }
}
