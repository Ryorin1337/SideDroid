// Where the picture goes when it is not the shape of the screen. The numbers match the host's kFill values.
package com.sidedroid.design

import kotlin.math.floor
import kotlin.math.max
import kotlin.math.min

const val FILL_FIT = 0
const val FILL_CROP = 1
const val FILL_STRETCH = 2
const val FILL_TILE = 3

val FillChoices = listOf(
    Choice("适应（留黑边）", FILL_FIT),
    Choice("填充（裁掉边缘）", FILL_CROP),
    Choice("拉伸（会变形）", FILL_STRETCH),
    Choice("平铺（原始大小，重复）", FILL_TILE),
)

data class PictureRect(val left: Float, val top: Float, val width: Float, val height: Float)

/** Where the whole picture lies, which with FILL_CROP is partly off the screen. Pure, for testing. */
fun pictureRect(fill: Int, panelW: Float, panelH: Float, pictureW: Int, pictureH: Int): PictureRect {
    if (fill == FILL_STRETCH || pictureW <= 0 || pictureH <= 0) return PictureRect(0f, 0f, panelW, panelH)
    val toWidth = panelW / pictureW
    val toHeight = panelH / pictureH
    val scale = if (fill == FILL_CROP) max(toWidth, toHeight) else min(toWidth, toHeight)
    val w = pictureW * scale
    val h = pictureH * scale
    return PictureRect((panelW - w) / 2, (panelH - h) / 2, w, h)
}

/** One copy of the picture: actual size, or smaller when it would not fit even once. Pure, for testing. */
fun tileSize(panelW: Int, panelH: Int, pictureW: Int, pictureH: Int): Pair<Int, Int> {
    if (pictureW <= 0 || pictureH <= 0) return max(panelW, 1) to max(panelH, 1)
    val scale = min(1f, min(panelW.toFloat() / pictureW, panelH.toFloat() / pictureH))
    return max((pictureW * scale).toInt(), 1) to max((pictureH * scale).toInt(), 1)
}

/** Which copy of the picture this position along one axis is in. Pure, for testing. */
fun tileIndex(position: Float, tile: Int): Int = floor(position / max(tile, 1)).toInt().coerceAtLeast(0)

/** How far along a given copy this position is, from 0 to 1. Outside it, its nearest edge. Pure, for testing. */
fun tileFraction(position: Float, tile: Int, index: Int): Float =
    ((position - index * tile) / max(tile, 1)).coerceIn(0f, 1f)
