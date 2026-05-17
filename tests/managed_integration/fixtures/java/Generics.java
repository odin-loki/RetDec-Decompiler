import java.util.*;

/**
 * Tier 4: Generics and collections. Tests generic classes, bounded type
 * parameters, wildcard usage, and standard collection operations.
 */
public class Generics {
    static class Pair<A, B> {
        final A first;
        final B second;
        Pair(A a, B b) { first = a; second = b; }
        @Override public String toString() { return "(" + first + "," + second + ")"; }
    }

    static <T extends Comparable<T>> T max(List<T> items) {
        T result = items.get(0);
        for (T item : items) {
            if (item.compareTo(result) > 0) result = item;
        }
        return result;
    }

    static double sumNumbers(List<? extends Number> nums) {
        double sum = 0;
        for (Number n : nums) sum += n.doubleValue();
        return sum;
    }

    public static void main(String[] args) {
        Pair<String, Integer> p = new Pair<>("hello", 42);
        System.out.println(p);

        List<Integer> ints = Arrays.asList(3, 1, 4, 1, 5, 9, 2, 6);
        System.out.println("max=" + max(ints));

        List<Double> doubles = Arrays.asList(1.1, 2.2, 3.3);
        System.out.println("sum=" + sumNumbers(doubles));

        Map<String, List<Integer>> grouped = new HashMap<>();
        grouped.computeIfAbsent("evens", k -> new ArrayList<>()).add(2);
        grouped.computeIfAbsent("evens", k -> new ArrayList<>()).add(4);
        System.out.println("grouped=" + grouped);
    }
}
