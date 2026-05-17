/**
 * Tier 2: Data classes, sealed classes, and when expressions.
 * Tests Kotlin-specific bytecode patterns (component functions, copy, etc.).
 */
data class Point(val x: Double, val y: Double) {
    fun distanceTo(other: Point): Double {
        val dx = x - other.x
        val dy = y - other.y
        return Math.sqrt(dx * dx + dy * dy)
    }
}

sealed class Shape {
    data class Circle(val radius: Double) : Shape()
    data class Rectangle(val width: Double, val height: Double) : Shape()
    object Empty : Shape()
}

fun area(shape: Shape): Double = when (shape) {
    is Shape.Circle    -> Math.PI * shape.radius * shape.radius
    is Shape.Rectangle -> shape.width * shape.height
    Shape.Empty        -> 0.0
}

fun main() {
    val p1 = Point(0.0, 0.0)
    val p2 = Point(3.0, 4.0)
    println("distance=${p1.distanceTo(p2)}")

    val shapes: List<Shape> = listOf(
        Shape.Circle(5.0),
        Shape.Rectangle(4.0, 6.0),
        Shape.Empty
    )
    shapes.forEach { println("area=${area(it)}") }

    val p3 = p2.copy(x = 10.0)
    println("copied=$p3")
}
