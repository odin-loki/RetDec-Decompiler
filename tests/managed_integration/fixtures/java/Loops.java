/**
 * Tier 2: Control flow. Tests for/while/do-while loops, break, continue,
 * integer arithmetic, and array access patterns.
 */
public class Loops {
    public static int sumArray(int[] arr) {
        int sum = 0;
        for (int i = 0; i < arr.length; i++) {
            sum += arr[i];
        }
        return sum;
    }

    public static int collatz(int n) {
        int steps = 0;
        while (n != 1) {
            if (n % 2 == 0) {
                n /= 2;
            } else {
                n = 3 * n + 1;
            }
            steps++;
        }
        return steps;
    }

    public static void main(String[] args) {
        int[] data = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        System.out.println("Sum: " + sumArray(data));
        System.out.println("Collatz(27): " + collatz(27));

        int i = 0;
        do {
            if (i == 5) { i++; continue; }
            if (i == 8) break;
            i++;
        } while (i < 10);
        System.out.println("i=" + i);
    }
}
