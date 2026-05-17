/**
 * Tier 3: Exception handling. Tests try/catch/finally, custom exceptions,
 * exception chaining, and multi-catch (Java 7+).
 */
public class Exceptions {
    static class AppException extends RuntimeException {
        private final int code;
        public AppException(String msg, int code) {
            super(msg);
            this.code = code;
        }
        public int getCode() { return code; }
    }

    static int divide(int a, int b) {
        if (b == 0) throw new AppException("Division by zero", 42);
        return a / b;
    }

    static String safeOp(int a, int b) {
        try {
            int result = divide(a, b);
            return "ok:" + result;
        } catch (AppException e) {
            return "app-err:" + e.getCode();
        } catch (ArithmeticException | IllegalArgumentException e) {
            return "math-err:" + e.getMessage();
        } finally {
            // always runs
        }
    }

    public static void main(String[] args) {
        System.out.println(safeOp(10, 2));
        System.out.println(safeOp(10, 0));

        try {
            throw new RuntimeException("outer", new AppException("inner", 99));
        } catch (RuntimeException e) {
            System.out.println("cause=" + ((AppException) e.getCause()).getCode());
        }
    }
}
