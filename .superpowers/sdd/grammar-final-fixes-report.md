# Grammar final fixes report
All six items done in one commit (see git log). 
- Item 2 RED: `morpheus_tests.exe "[gbnf]"` -> 1 failed (GbnfTests.cpp:137 no exception thrown). GREEN: all passed (99 assertions in 11 test cases).
- Item 3: Schema.h and spec section 4 now describe the lone-surrogate/parse-failure host duty and grammar-enforced key order; Schema.h writes it as two backslashes + ud800 (ASCII).
- Item 6: consumer-check configure now fails without Morpheus::grammar.
- ctest --preset tests: 100% passed out of 368. Consumer check from scratch: "consumer_check: ok (rolled 15, total 18)".
- Byte checks (python, bytes >127 / control bytes) on all 7 changed files: 0 / 0 each (bash grep with \x00 range was invalid, so python used).
