import java.util.*;
import java.util.function.*;
import java.util.stream.*;

/**
 * Tier 5: Lambdas and streams. Tests lambda syntax, method references,
 * functional interfaces, and the Stream API (Java 8+).
 */
public class Lambdas {
    @FunctionalInterface
    interface Transformer<T, R> {
        R transform(T input);
    }

    static <T, R> List<R> mapList(List<T> list, Transformer<T, R> fn) {
        List<R> result = new ArrayList<>();
        for (T item : list) result.add(fn.transform(item));
        return result;
    }

    public static void main(String[] args) {
        // Basic lambda
        Runnable r = () -> System.out.println("lambda ran");
        r.run();

        // Method reference
        List<String> words = Arrays.asList("hello", "world", "java");
        words.forEach(System.out::println);

        // Custom functional interface
        Transformer<String, Integer> len = String::length;
        List<Integer> lengths = mapList(words, len);
        System.out.println("lengths=" + lengths);

        // Stream pipeline
        List<Integer> nums = Arrays.asList(1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
        int sumEvens = nums.stream()
            .filter(n -> n % 2 == 0)
            .mapToInt(Integer::intValue)
            .sum();
        System.out.println("sum_evens=" + sumEvens);

        // Closures capture
        int factor = 3;
        List<Integer> tripled = nums.stream()
            .map(n -> n * factor)
            .collect(Collectors.toList());
        System.out.println("tripled[0..2]=" + tripled.subList(0, 3));

        // Comparator composition
        List<String> sorted = words.stream()
            .sorted(Comparator.comparingInt(String::length).reversed())
            .collect(Collectors.toList());
        System.out.println("sorted=" + sorted);
    }
}
