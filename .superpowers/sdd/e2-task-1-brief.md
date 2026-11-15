### Task 1: The host's clock

**Files:**
- Modify: `src/turn/HostedWorld.h`, `src/turn/HostedWorld.cpp`
- Modify: `tests/hosted/fixtures/README.md` (the `clock` field of `open.json`'s format)
- Test: `tests/hosted/HostedWorldTests.cpp`

**Interfaces:**
- Consumes: nothing new.
- Produces: `std::string turn::HostData::clock = {};` (empty: no clock; `HostDataFromJson` reads `"clock"`, `HostDataToJson` writes it when set); `std::optional<double> turn::Observation::clock = {};`; `bool turn::HostedWorld::HasClock() const`; `HostedWorld`'s constructor refuses a clock that is not a well-formed JSON pointer (`DataError` "host data at /clock: ..."); `Observe` reports a fault at the clock's pointer when it holds no finite number.

- [ ] **Step 1: Write the failing tests**

Add to `tests/hosted/HostedWorldTests.cpp` (it has `test::FixtureHostData()` and `test::FixtureObservation()`; observation.json has `game_time.days_passed`, a number):

```cpp
TEST_CASE("A host clock is read from every observation, and refused when missing or not a number", "[hosted][clock]")
{
    turn::HostData data = test::FixtureHostData();
    CHECK_FALSE(turn::HostedWorld(data).HasClock());
    CHECK_FALSE(turn::HostedWorld(data).Observe(test::FixtureObservation()).clock.has_value());

    data.clock = "/game_time/days_passed";
    const turn::HostedWorld world(data);
    CHECK(world.HasClock());
    const std::optional<double> clock = world.Observe(test::FixtureObservation()).clock;
    REQUIRE(clock.has_value());
    CHECK(*clock == test::FixtureObservation().at("game_time").at("days_passed").get<double>());

    nlohmann::json missing = test::FixtureObservation();
    missing.at("game_time").erase("days_passed");
    CHECK_THROWS_WITH(world.Observe(missing), ContainsSubstring("/game_time/days_passed") &&
                                                  ContainsSubstring("expected the clock, a number"));
    nlohmann::json text = test::FixtureObservation();
    text["game_time"]["days_passed"] = "noon";
    CHECK_THROWS_AS(world.Observe(text), core::DataError);
}

TEST_CASE("A host clock must be a JSON pointer, and round-trips through the host data's JSON", "[hosted][clock]")
{
    nlohmann::json open = test::ReadFixture("open.json");
    open["clock"] = "game_time/days_passed"; // No leading '/'.
    CHECK_THROWS_WITH(turn::HostedWorld(turn::HostDataFromJson(open)), ContainsSubstring("/clock"));
    open["clock"] = 7;
    CHECK_THROWS_WITH(turn::HostDataFromJson(open), ContainsSubstring("/clock"));
    open["clock"] = "/game_time/days_passed";
    const turn::HostData data = turn::HostDataFromJson(open);
    CHECK(data.clock == "/game_time/days_passed");
    CHECK(turn::HostDataToJson(data).at("clock") == "/game_time/days_passed");
    CHECK_FALSE(turn::HostDataToJson(test::FixtureHostData()).contains("clock"));
}
```

(The schema check runs after the core in `Observe`; a host schema that also refuses the missing field adds its own fault, which is fine: the test checks one message is in it. If observation.json's schema requires `days_passed`, the `missing` case still carries the clock fault.)

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset tests`
Expected: a compile error (`'clock': is not a member of 'morpheus::turn::HostData'`).

- [ ] **Step 3: The clock**

In `src/turn/HostedWorld.h`: `HostData` gains, after `lore`:

```cpp
    // The game's clock (rollback spec, section 1): a JSON pointer to a number in every observation, e.g.
    // "/game_time/days_passed". Empty: no clock, and a run cannot be rewound.
    std::string clock = {};
```

`Observation` gains, after `actors`: `std::optional<double> clock = {}; // The observation's game time, when the host data names a clock.` `HostedWorld` gains `bool HasClock() const;` and a private member `nlohmann::json::json_pointer _clock = {}; // Empty: no clock.` with a `bool _hasClock = false;` beside it (a default pointer is the whole document, so the flag says whether one was named).

Update the comments of `HostDataToJson` / `HostDataFromJson`: the form gains an optional `"clock"` string.

In `src/turn/HostedWorld.cpp`:

- `HostDataFromJson` accepts the key `"clock"` (add it to the allowed keys); it must be a string, else `fault("/clock", "'clock' must be a JSON pointer string")`.
- `HostDataToJson` writes `"clock"` only when it is not empty.
- The constructor, after the existing checks:

```cpp
    // The clock, when named: a well-formed JSON pointer, read from every observation.
    if (!data.clock.empty()) {
        try {
            _clock = nlohmann::json::json_pointer(data.clock);
        } catch (const nlohmann::json::exception& error) {
            throw core::DataError(std::format("host data at /clock: '{}' is not a JSON pointer: {}", data.clock,
                                              error.what()));
        }
        _hasClock = true;
    }
```

- `Observe`, after the schema faults and before throwing:

```cpp
    // The clock, when the host names one: a finite number at its pointer.
    std::optional<double> clock;
    if (_hasClock) {
        const bool present = observation.contains(_clock) && observation.at(_clock).is_number() &&
                             std::isfinite(observation.at(_clock).get<double>());
        if (present) {
            clock = observation.at(_clock).get<double>();
        } else {
            read.faults.push_back({_clock.to_string(), "expected the clock, a number"});
        }
    }
```

  and return `Observation{observation, std::move(read.actors), clock}`. (`ObservationError` already renders each fault as "observation at <pointer>: <problem>".) Add `<cmath>` for `std::isfinite`.

- `bool HostedWorld::HasClock() const { return _hasClock; }`

In `tests/hosted/fixtures/README.md`, under `open.json`, add: the optional `clock`, a JSON pointer to a number in every observation (rollback spec, section 1).

- [ ] **Step 4: Run the tests to see them pass**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[hosted]"`, `"[hosted-server]"`
Expected: `All tests passed`.

- [ ] **Step 5: Commit**

```bash
git add src/turn/HostedWorld.h src/turn/HostedWorld.cpp tests/hosted/
git commit -m "The host's clock: a JSON pointer in the host data, read from every observation"
```

---

