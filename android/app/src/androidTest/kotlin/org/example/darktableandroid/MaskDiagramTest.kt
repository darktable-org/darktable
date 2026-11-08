/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertTextContains
import androidx.compose.ui.test.hasSetTextAction
import androidx.compose.ui.test.hasText
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performTextReplacement
import androidx.test.platform.app.InstrumentationRegistry
import org.example.darktableandroid.nativecore.ModuleParameter
import org.junit.Rule
import org.junit.Test
import org.junit.Assert.assertEquals

class MaskDiagramTest {
    @get:Rule val compose = createComposeRule()
    private val context get() = InstrumentationRegistry.getInstrumentation().targetContext

    private fun field(id: Int, name: String, value: Double, shape: String) =
        ModuleParameter("mask.$id.$name", "$shape $id · $name", "float", value, null, null, emptyList())

    @Test fun nativeShapeSelectionKeepsDiagramAndAccessibleCoordinatesTogether() {
        val parameters = listOf(
            field(15, "x", 0.25, "circle"), field(15, "y", 0.5, "circle"), field(15, "radius", 0.1, "circle"),
            field(15, "source_x", 0.75, "circle"), field(15, "source_y", 0.5, "circle"),
            field(27, "x", 0.7, "ellipse"), field(27, "y", 0.4, "ellipse"),
            field(27, "radius_x", 0.2, "ellipse"), field(27, "radius_y", 0.1, "ellipse"),
        )
        compose.setContent { MaterialTheme { MaskDiagram(parameters) } }
        val circleDescription = context.getString(R.string.mask_diagram_description, "circle 15", "0.25", "0.5") + " " +
            context.getString(R.string.mask_diagram_source, "0.75", "0.5")
        compose.onNodeWithContentDescription(circleDescription).assertIsDisplayed()
        compose.onNodeWithText("circle 15").performClick()
        compose.onNodeWithText("ellipse 27").performClick()
        compose.onNodeWithContentDescription(context.getString(R.string.mask_diagram_description, "ellipse 27", "0.7", "0.4"))
            .assertIsDisplayed()
    }

    private fun pathNodes(count: Int, firstX: Double = 0.2): List<ModuleParameter> = (0 until count).flatMap { index ->
        val x = firstX + index * 0.1
        val y = 0.2 + index * 0.1
        listOf("x" to x, "y" to y, "ctrl1_x" to x - 0.02, "ctrl1_y" to y - 0.02,
            "ctrl2_x" to x + 0.02, "ctrl2_y" to y + 0.02, "border_in" to 0.03, "border_out" to 0.03).map { (name, value) ->
            field(40, "node.$index.$name", value, "path")
                .copy(minimum = if(name.startsWith("ctrl")) -1.0 else 0.0, maximum = 2.0)
        } + ModuleParameter("mask.40.node.$index.insert_after", "Insert after node ${index + 1}", "action",
            1.0, 1.0, 1.0, emptyList())
    }

    @Test fun selectedNodeActionAddressesTheNativeNodeWithoutEditingOtherNodes() {
        val parameters = pathNodes(4)
        val edits = mutableListOf<Pair<String, Double>>()
        val visible = parameters.filter { it.name.endsWith("insert_after") }.map { it.name }.toSet()
        compose.setContent { MaterialTheme { MaskControls(parameters, true, visible) { name, value -> edits += name to value } } }
        compose.onNodeWithContentDescription(context.getString(R.string.mask_path_select_node)).performClick()
        compose.onNodeWithText(context.getString(R.string.mask_path_node, 3)).performClick()
        compose.onNodeWithText("Insert after node 1").assertDoesNotExist()
        compose.onNodeWithText("Insert after node 3").performClick()
        compose.runOnIdle { assertEquals(listOf("mask.40.node.2.insert_after" to 1.0), edits) }
    }

    @Test fun queryLimitsControlsWithoutLosingAppliedPathGeometry() {
        val parameters = pathNodes(4)
        val selected = "mask.40.node.2.x"
        compose.setContent { MaterialTheme { MaskControls(parameters, true, setOf(selected)) { _, _ -> } } }
        compose.onNodeWithContentDescription(context.getString(R.string.mask_path_diagram_description,
            "path 40", 4, 3, "0.4", "0.4")).assertIsDisplayed()
        compose.onNodeWithText("path 40 · node.0.x").assertDoesNotExist()
        compose.onNode(hasSetTextAction() and hasText("path 40 · node.2.x")).assertIsDisplayed()
    }

    @Test fun changedNativeTopologyDiscardsDraftForAReusedNodeIndex() {
        val parameters = mutableStateOf(pathNodes(4))
        val visible = setOf("mask.40.node.1.x")
        compose.setContent { MaterialTheme { MaskControls(parameters.value, true, visible) { _, _ -> } } }
        val input = compose.onNode(hasSetTextAction() and hasText("path 40 · node.1.x"))
        input.performTextReplacement("0.95")
        compose.runOnIdle { parameters.value = pathNodes(3, firstX = 0.5) }
        input.assertTextContains("0.6")
        compose.onNodeWithText("0.95").assertDoesNotExist()
    }
}
