# Task 4 report
Implemented per brief: AppController owns shared_ptr<ModelSeat>, wraps factory in ModelSeatBackend, exposes Seat().
RED: `cmake --build --preset tests` -> error C2039: 'Seat': is not a member of 'morpheus::app::AppController' (AppControllerTests.cpp).
GREEN: `morpheus_tests.exe "[app_controller],[engine_host],[ui]"` -> All tests passed (400 assertions in 18 test cases); `ctest --preset tests` -> 100% passed out of 306. `build/dev --target morpheus` built.
Files: src/app/model/AppController.{h,cpp}, tests/app/AppControllerTests.cpp.
Deviations: none (brief's make_unique arg order matches the actual ModelSeatBackend ctor (inner, seat)). No racy test found.
