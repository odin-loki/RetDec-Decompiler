; ModuleID = 'fixture'
source_filename = "fixture"

@str_fib = constant [14 x i8] c"fib(10) = %d\0A\00"

define i32 @fib(i32 %n) {
dec_label_pc_401000:
  %0 = icmp slt i32 %n, 2
  br i1 %0, label %dec_label_pc_401010, label %dec_label_pc_401018

dec_label_pc_401010:
  ret i32 %n

dec_label_pc_401018:
  %1 = add i32 %n, -1
  %2 = call i32 @fib(i32 %1)
  %3 = add i32 %n, -2
  %4 = call i32 @fib(i32 %3)
  %5 = add i32 %4, %2
  ret i32 %5
}

define i32 @main(i32 %argc, i8** %argv) {
dec_label_pc_401030:
  %0 = call i32 @fib(i32 10)
  ret i32 0
}
