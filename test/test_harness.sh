#!/usr/bin/env bash
# test_harness.sh

echo "[DEBUG_SOURCE] Starting to source test_harness.sh" >&2

# These variables are expected to be set by the calling script/environment
# SIMPLE9P_BINARY, NINEP_FUSE_BINARY, HARNESS_DEBUG, HARNESS_PORT_BASE, HARNESS_RESULTS_ROOT_DIR

_HARNESS_TEMP_DIR=""
_HARNESS_SERVER_PID=""
_HARNESS_CURRENT_PORT=""
_HARNESS_TCP_ADDRESS=""
_HARNESS_FUSE_MOUNT_PATH_FULL=""
NO_MOUNT=0
_CURRENT_TEST_RUN_ARCHIVE_DIR=""

_harness_log() {
    if [[ "${HARNESS_DEBUG:-0}" -ne 0 ]]; then
        # Using printf for more robust output, especially if $1 contains %
        printf "[HARNESS_DEBUG] %s - PID:%s - %s\n" "$(date '+%Y-%m-%d %H:%M:%S')" "$$" "$1" >&2
    fi
}
export -f _harness_log # Export it
echo "[DEBUG_SOURCE] Defined _harness_log" >&2

test_list() {
    local search_root="${1:-.}"
    find "$search_root" -type d -name 'test_*' -exec test -f '{}/test.sh' \; -print
}
export -f test_list
echo "[DEBUG_SOURCE] Defined test_list" >&2

test_setup() {
    local test_case_path="$1"
    _harness_log "Setting up test: $test_case_path using port $_HARNESS_CURRENT_PORT"

    _HARNESS_TEMP_DIR=$(mktemp -d "simple9ptest_$(basename "$test_case_path")_XXXXXX")
    if [[ -z "$_HARNESS_TEMP_DIR" || ! -d "$_HARNESS_TEMP_DIR" ]]; then
        echo "[HARNESS_ERROR] Failed to create temporary directory." >&2
        return 1
    fi
    _harness_log "Temp dir: $_HARNESS_TEMP_DIR"

    # Ensure paths are absolute for robustness before cp
    local abs_test_case_path
    abs_test_case_path=$(realpath "$test_case_path")

    cp -aT "$abs_test_case_path" "$_HARNESS_TEMP_DIR/" || { # Use absolute source path
        echo "[HARNESS_ERROR] Failed to copy test case contents from '$abs_test_case_path' to '$_HARNESS_TEMP_DIR'." >&2
        rm -rf "$_HARNESS_TEMP_DIR"; return 1; }

    cd "$_HARNESS_TEMP_DIR" || {
        echo "[HARNESS_ERROR] Failed to cd into '$_HARNESS_TEMP_DIR'." >&2
        rm -rf "$_HARNESS_TEMP_DIR"; return 1; }
    _harness_log "Current dir: $(pwd)"
    NO_MOUNT=0
    if [[ -f "./setup.sh" ]]; then
        _harness_log "Sourcing setup.sh..."
        source "./setup.sh"
        local setup_ret=$?
        if [[ $setup_ret -ne 0 ]]; then
            echo "[HARNESS_ERROR] setup.sh failed with code $setup_ret." >&2
            cd - >/dev/null; rm -rf "$_HARNESS_TEMP_DIR"; return 1; fi
        _harness_log "setup.sh sourced. NO_MOUNT is now $NO_MOUNT"
    fi
    test_data_create; return $?
}
export -f test_setup
echo "[DEBUG_SOURCE] Defined test_setup" >&2

test_data_create() {
    _harness_log "Creating data directories (expected, actual, mount)..."
    if [[ -d "./data" ]]; then
        _harness_log "Moving './data' to './expected' and copying to './actual'."
        mv "./data" "./expected" || { echo "[HARNESS_ERROR] Failed to mv data to expected." >&2; return 1; }
        cp -aT "./expected" "./actual" || { echo "[HARNESS_ERROR] Failed to cp expected to actual." >&2; return 1; }
    else
        _harness_log "No './data' directory found. Creating empty './expected' and './actual'."
        mkdir "./expected" "./actual" || { echo "[HARNESS_ERROR] Failed to create expected/actual." >&2; return 1; }
    fi
    mkdir "./mount" || { echo "[HARNESS_ERROR] Failed to create mount directory." >&2; return 1; }
    _harness_log "Data directories created."
    return 0
}
export -f test_data_create
echo "[DEBUG_SOURCE] Defined test_data_create" >&2

_wait_for_tcp_port() {
    local host="$1"; local port="$2"; local timeout_seconds="${3:-5}"
    _harness_log "Waiting for $host:$port to be connectable (timeout: ${timeout_seconds}s)..."
    if command -v nc >/dev/null 2>&1; then
        for ((i=0; i<timeout_seconds*10; i++)); do
            nc -z "$host" "$port" >/dev/null 2>&1 && _harness_log "$host:$port is connectable." && return 0
            sleep 0.1; done
    else
        _harness_log "nc command not found, using simple sleep to wait for server."; sleep "$((timeout_seconds > 2 ? 2 : timeout_seconds))"; fi
    _harness_log "Timeout or unable to check port for $host:$port."
    if ! kill -0 "$_HARNESS_SERVER_PID" 2>/dev/null; then
        _harness_log "Server PID $_HARNESS_SERVER_PID seems to have died before port check."; return 1; fi
    if command -v nc >/dev/null 2>&1; then return 1; else return 0; fi
}
export -f _wait_for_tcp_port
echo "[DEBUG_SOURCE] Defined _wait_for_tcp_port" >&2

test_connect() {
    if [[ "$NO_MOUNT" -eq 1 ]]; then _harness_log "NO_MOUNT is set. Skipping server and FUSE."; return 0; fi
    _harness_log "Connecting 9P server (port $_HARNESS_CURRENT_PORT) and FUSE client..."
    _HARNESS_FUSE_MOUNT_PATH_FULL="$(pwd)/mount"
    local abs_actual_path; abs_actual_path=$(realpath "./actual") # Ensure server gets absolute path

    _harness_log "Starting server: $SIMPLE9P_BINARY -p \"$_HARNESS_TCP_ADDRESS\" \"$abs_actual_path\""
    "$SIMPLE9P_BINARY" -p "$_HARNESS_TCP_ADDRESS" "$abs_actual_path" &
    _HARNESS_SERVER_PID=$!
    _harness_log "Server PID: $_HARNESS_SERVER_PID"
    if ! _wait_for_tcp_port "localhost" "$_HARNESS_CURRENT_PORT"; then
        if kill -0 "$_HARNESS_SERVER_PID" 2>/dev/null; then
            echo "[HARNESS_ERROR] Server (PID $_HARNESS_SERVER_PID) started but port $_HARNESS_CURRENT_PORT not listening." >&2
            kill "$_HARNESS_SERVER_PID"; wait "$_HARNESS_SERVER_PID" 2>/dev/null
        else echo "[HARNESS_ERROR] Server (PID $_HARNESS_SERVER_PID) failed to start." >&2; fi
        _HARNESS_SERVER_PID=""; return 1; fi
    _harness_log "Mounting FUSE: $NINEP_FUSE_BINARY \"$_HARNESS_TCP_ADDRESS\" \"$_HARNESS_FUSE_MOUNT_PATH_FULL\""
    "$NINEP_FUSE_BINARY" "$_HARNESS_TCP_ADDRESS" "$_HARNESS_FUSE_MOUNT_PATH_FULL"
    local fuse_ret=$?
    if [[ $fuse_ret -ne 0 ]]; then
        echo "[HARNESS_ERROR] 9pfuse failed (code $fuse_ret) for $_HARNESS_TCP_ADDRESS." >&2
        if [[ -n "$_HARNESS_SERVER_PID" ]] && kill -0 "$_HARNESS_SERVER_PID" 2>/dev/null; then
            kill "$_HARNESS_SERVER_PID"; wait "$_HARNESS_SERVER_PID" 2>/dev/null; fi
        _HARNESS_SERVER_PID=""; return 1; fi
    _harness_log "FUSE mounted."; return 0
}
export -f test_connect
echo "[DEBUG_SOURCE] Defined test_connect" >&2

test_disconnect() {
    _harness_log "Disconnecting (PID: '$_HARNESS_SERVER_PID', Port: '$_HARNESS_CURRENT_PORT', Mount: '$_HARNESS_FUSE_MOUNT_PATH_FULL'). NO_MOUNT='${NO_MOUNT}'"
    if [[ "$NO_MOUNT" -eq 1 && -z "$_HARNESS_SERVER_PID" ]]; then
        _harness_log "NO_MOUNT was set and no server PID. Nothing to disconnect."; return 0; fi

    local fusermount_cmd=""
    if command -v fusermount3 >/dev/null 2>&1; then fusermount_cmd="fusermount3";
    elif command -v fusermount >/dev/null 2>&1; then fusermount_cmd="fusermount"; fi

    if [[ -n "$_HARNESS_FUSE_MOUNT_PATH_FULL" ]] && mountpoint -q "$_HARNESS_FUSE_MOUNT_PATH_FULL" 2>/dev/null ; then
        if [[ -n "$fusermount_cmd" ]]; then
            _harness_log "Unmounting FUSE from $_HARNESS_FUSE_MOUNT_PATH_FULL using $fusermount_cmd"
            "$fusermount_cmd" -u "$_HARNESS_FUSE_MOUNT_PATH_FULL" 2>/dev/null || _harness_log "WARN: $fusermount_cmd failed."
        else _harness_log "WARN: fusermount command not found."; fi
    else _harness_log "FUSE mount path '$_HARNESS_FUSE_MOUNT_PATH_FULL' not set, not mounted, or NO_MOUNT was true."; fi

    if [[ -n "$_HARNESS_SERVER_PID" ]] && kill -0 "$_HARNESS_SERVER_PID" 2>/dev/null; then
        _harness_log "Stopping server PID $_HARNESS_SERVER_PID (port $_HARNESS_CURRENT_PORT)..."
        kill "$_HARNESS_SERVER_PID"; local countdown=10
        while kill -0 "$_HARNESS_SERVER_PID" 2>/dev/null && [[ $countdown -gt 0 ]]; do sleep 0.1; countdown=$((countdown - 1)); done
        if kill -0 "$_HARNESS_SERVER_PID" 2>/dev/null; then _harness_log "SIGKILLing PID $_HARNESS_SERVER_PID."; kill -9 "$_HARNESS_SERVER_PID"; sleep 0.1;
        else _harness_log "Server PID $_HARNESS_SERVER_PID terminated."; fi
        wait "$_HARNESS_SERVER_PID" 2>/dev/null; _harness_log "Server PID $_HARNESS_SERVER_PID reaped.";
    else _harness_log "Server PID '$_HARNESS_SERVER_PID' not found or already stopped."; fi

    if [[ "$NO_MOUNT" -eq 0 && -n "$_HARNESS_CURRENT_PORT" ]] && command -v lsof >/dev/null 2>&1; then
        local lingering_pids; lingering_pids=$(lsof -t -i TCP:"$_HARNESS_CURRENT_PORT" -s TCP:LISTEN 2>/dev/null)
        if [[ -n "$lingering_pids" ]]; then
            for lingering_pid in $lingering_pids; do
                if [[ "$lingering_pid" != "$_HARNESS_SERVER_PID" ]] || ! kill -0 "$lingering_pid" 2>/dev/null ; then
                    _harness_log "WARN: lsof found lingering PID $lingering_pid on port $_HARNESS_CURRENT_PORT. SIGKILLing."; kill -9 "$lingering_pid" 2>/dev/null; fi
            done; fi; fi
    _HARNESS_SERVER_PID=""; _HARNESS_FUSE_MOUNT_PATH_FULL=""
    _harness_log "Disconnect actions complete."
}
export -f test_disconnect
echo "[DEBUG_SOURCE] Defined test_disconnect" >&2

test_run() {
    _harness_log "Running test script logic for port $_HARNESS_CURRENT_PORT..."
    _harness_log "Executing test.sh against './expected'..."
    ( set -e; cd "./expected" || { _harness_log "Cannot cd ./expected"; exit 126; }; "../test.sh" > "../expected.stdout" 2> "../expected.stderr"; echo $? > "../expected.return")
    _harness_log "'expected' run complete. Return: $(cat ./expected.return 2>/dev/null || echo 'N/A')"

    if [[ "$NO_MOUNT" -eq 1 ]]; then
        _harness_log "NO_MOUNT=1. Running test.sh against './actual' directly."
        ( set -e; cd "./actual" || { _harness_log "Cannot cd ./actual"; exit 126; }; "../test.sh" > "../actual.stdout" 2> "../actual.stderr"; echo $? > "../actual.return")
        _harness_log "'actual' run complete (NO_MOUNT). Return: $(cat ./actual.return 2>/dev/null || echo 'N/A')"
    else
        _harness_log "Executing test.sh against './mount' (actual via FUSE)..."
        (
            trap 'COMMAND_EXIT_CODE=$?; _harness_log "Subshell EXIT trap (code: $COMMAND_EXIT_CODE) in test_run for actual - calling test_disconnect"; test_disconnect; exit $COMMAND_EXIT_CODE' EXIT SIGINT SIGTERM
            if ! test_connect; then
                echo "[HARNESS_ERROR] Connection phase failed for actual run on port $_HARNESS_CURRENT_PORT." >&2
                echo "Connection failed" > "./actual.stderr"; echo "1" > "./actual.return"; touch "./actual.stdout"
                exit 1 # Will trigger trap above
            fi
            _harness_log "Connected for actual run. Executing test script..."
            ( set -e; cd "./mount" || { _harness_log "Cannot cd ./mount"; exit 126; }; "../test.sh" > "../actual.stdout" 2> "../actual.stderr"; echo $? > "../actual.return")
            _harness_log "'actual' run complete. Return: $(cat ./actual.return 2>/dev/null || echo 'N/A')"
        ) # Disconnect called by trap from this subshell
    fi
    _harness_log "Test script execution phase complete."
    return 0
}
export -f test_run
echo "[DEBUG_SOURCE] Defined test_run" >&2

test_gather() {
    local target_name="$1"; local target_data_dir="./$target_name"
    _harness_log "Gathering data for '$target_name' from dir '$target_data_dir' and files './${target_name}.*'"
    echo "#### [TEST RESULT START/END: stdout for ${target_name} ####"
    cat "./${target_name}.stdout" 2>/dev/null || echo "(stdout for ${target_name} N/A)"
    echo "#### [TEST RESULT START/END: stderr for ${target_name} ####"
    cat "./${target_name}.stderr" 2>/dev/null || echo "(stderr for ${target_name} N/A)"
    echo "#### [TEST RESULT START/END: return_code for ${target_name} ####"
    cat "./${target_name}.return" 2>/dev/null || echo "(return_code for ${target_name} N/A)"

    if [[ -d "$target_data_dir" ]]; then
        echo "#### [TEST RESULT START/END: directory_listing for ${target_name} ####"
        ( cd "$target_data_dir" || { echo "ERROR: Cannot cd $target_data_dir"; exit 1; }
          # Use find to list all items, then stat each one.
          # Sort for consistent order.
          # Remove volatile fields from stat output.
          find . -print0 | sort -z | while IFS= read -r -d $'\0' entry; do
            local clean_entry="${entry#./}"; [[ -z "$clean_entry" ]] && clean_entry="."
            echo "#### [TEST RESULT START/END: entry_details for ${target_name}/${clean_entry} ####"
            echo "Path: $clean_entry"
            stat_output=$(stat "$entry" 2>/dev/null)
            if [[ $? -eq 0 ]]; then
                # Attempt to normalize stat output by removing volatile parts
                # This is a best-effort and might need adjustment based on specific 'stat' version
                echo "$stat_output" | \
                sed -e 's/Inode: [0-9]\+//g' \
                    -e 's/Device: [0-9a-fA-F]*h\/[0-9]*d//g' \
                    -e 's/Links: [0-9]*//g' \
                    -e 's/[0-9]\{4\}-[0-9]\{2\}-[0-9]\{2\} [0-9]\{2\}:[0-9]\{2\}:[0-9]\{2\}\(\.[0-9]*\)\{0,1\} [+-][0-9]\{4\}//g' \
                    -e 's/Birth: -//g' \
                    -e 's/Birth: [0-9]\{4\}.*//g' \
                    -e '/^[[:space:]]*$/d' \
                    -e 's/Access: [0-9]\{4\}.*//g' \
                    -e 's/Modify: [0-9]\{4\}.*//g' \
                    -e 's/Change: [0-9]\{4\}.*//g'
            else
                echo "Stat failed for $clean_entry"
            fi

            if [[ -f "$entry" && ! -L "$entry" ]]; then
                echo "#### [TEST RESULT START/END: hexdump for ${target_name}/${clean_entry} ####"
                xxd "$entry" 2>/dev/null || echo "Hexdump failed for $clean_entry"
            elif [[ -L "$entry" ]]; then
                echo "#### [TEST RESULT START/END: symlink_target for ${target_name}/${clean_entry} ####"
                readlink "$entry" 2>/dev/null || echo "Readlink failed for $clean_entry"
            fi
            echo "#### [TEST RESULT START/END: end of entry_details for ${target_name}/${clean_entry} ####"
          done
        )
        echo "#### [TEST RESULT START/END: disk_usage for ${target_name} ####"
        # Get just the size in bytes for the directory itself
        du -bs "$target_data_dir" 2>/dev/null | awk '{print $1}' || echo "(du failed for ${target_name})"
    else
        _harness_log "Dir '$target_data_dir' not found for gather."
        echo "#### [TEST RESULT START/END: directory_listing for ${target_name} ####"
        echo "(directory N/A for ${target_name})"
        echo "#### [TEST RESULT START/END: disk_usage for ${target_name} ####"
        echo "(disk_usage N/A for ${target_name})"
    fi
    echo "#### [TEST RESULT START/END: end of gather for ${target_name} ####"
    return 0
}
export -f test_gather
echo "[DEBUG_SOURCE] Defined test_gather" >&2

test_diff() {
    _harness_log "Diffing results..."
    # Generate the full output files
    test_gather "expected" > "./expected.all.raw" 2>&1 || { echo "Gather expected failed" > "./expected.all.raw"; }
    test_gather "actual" > "./actual.all.raw" 2>&1 || { echo "Gather actual failed" > "./actual.all.raw"; }

    # Create filtered versions for diffing
    # Also normalize:
    # - Link counts in ls -la output (2nd field after permissions)
    # - "total N" lines from ls
    # - Temp dir paths (replace /path/to/temp/expected or /path/to/temp/mount with TESTROOT)
    grep -v "#### \[TEST RESULT START/END:" "./expected.all.raw" | \
        sed -E 's/^([-dlrwxsStT]{10}) +[0-9]+ /\1 NLINK /g' | \
        sed -E 's/^total [0-9]+$/total BLOCKS/' | \
        sed -E 's|/[^ ]*/simple9ptest_[^/]+/(expected\|mount\|actual)|TESTROOT|g' > "./expected.all"
    grep -v "#### \[TEST RESULT START/END:" "./actual.all.raw" | \
        sed -E 's/^([-dlrwxsStT]{10}) +[0-9]+ /\1 NLINK /g' | \
        sed -E 's/^total [0-9]+$/total BLOCKS/' | \
        sed -E 's|/[^ ]*/simple9ptest_[^/]+/(expected\|mount\|actual)|TESTROOT|g' > "./actual.all"

    # Perform the diff on filtered files
    if diff -u "./expected.all" "./actual.all" > "./results.diff"; then
        _harness_log "No differences. PASS.";
        # Clean up intermediate raw files if test passes and not in debug mode
        if [[ "${HARNESS_DEBUG:-0}" -eq 0 ]]; then
            rm -f "./expected.all.raw" "./actual.all.raw" "./expected.all" "./actual.all"
        fi
        return 0;
    else
        echo "[HARNESS_INFO] Differences found. FAIL. See results.diff (filtered) and .all.raw files (unfiltered)." >&2;
        cat "./results.diff" >&2; # Output filtered diff to stderr for immediate visibility
        # Keep .all.raw and .all files for debugging failed tests
        return 1;
    fi
}
export -f test_diff
echo "[DEBUG_SOURCE] Defined test_diff" >&2

test_run_one() {
    local test_case_dir="$1"; local current_test_port="$2"; local main_run_archive_dir_abs="$3"
    local test_name; test_name=$(basename "$test_case_dir")
    local original_pwd; original_pwd=$(pwd)
    local test_specific_archive_dir; test_specific_archive_dir=$(realpath -m "$main_run_archive_dir_abs/$test_name")

    echo "-----------------------------------------------------"
    printf "STARTING TEST: %s (Port: %s, Results: %s)\n" "$test_name" "$current_test_port" "$test_specific_archive_dir"
    _harness_log "Original PWD: $original_pwd. Main archive for this run: $main_run_archive_dir_abs"
    mkdir -p "$test_specific_archive_dir" || { echo "[HARNESS_ERROR] CRITICAL: Cannot create $test_specific_archive_dir" >&2; return 1; }
    _harness_log "Ensured test specific archive dir exists: $test_specific_archive_dir"

    (
        _HARNESS_TEMP_DIR=""; _HARNESS_SERVER_PID=""; _HARNESS_FUSE_MOUNT_PATH_FULL=""
        _HARNESS_CURRENT_PORT="$current_test_port"; _HARNESS_TCP_ADDRESS="tcp!localhost!$_HARNESS_CURRENT_PORT"
        local final_exit_code=1 # Assume failure

        trap 'COMMAND_EXIT_CODE=$?; _harness_log "Subshell trap in test_run_one for $test_name (exit $COMMAND_EXIT_CODE) - calling test_disconnect"; test_disconnect; _harness_log "Trap: cd $original_pwd"; cd "$original_pwd" 2>/dev/null || true; if [[ "${HARNESS_DEBUG:-0}" -eq 0 && $final_exit_code -eq 0 && -n "$_HARNESS_TEMP_DIR" && -d "$_HARNESS_TEMP_DIR" ]]; then _harness_log "Trap: Cleaning up $_HARNESS_TEMP_DIR"; rm -rf "$_HARNESS_TEMP_DIR"; elif [[ -n "$_HARNESS_TEMP_DIR" ]]; then _harness_log "Trap: Leaving $_HARNESS_TEMP_DIR"; fi; exit $COMMAND_EXIT_CODE' EXIT SIGINT SIGTERM

        if ! test_setup "$test_case_dir"; then echo "[HARNESS_RESULT] $test_name: SETUP_FAIL" >&2; echo "SETUP_FAIL" > "$test_specific_archive_dir/HARNESS_STATUS.txt"; exit 1; fi
        _harness_log "Test setup complete. In temp dir: $(pwd)"

        if ! test_run; then _harness_log "test_run had an issue for $test_name."; fi
        _harness_log "Ensuring final disconnect for $test_name after test_run."
        if [[ "$NO_MOUNT" -eq 0 && (-n "$_HARNESS_SERVER_PID" || -n "$_HARNESS_FUSE_MOUNT_PATH_FULL") ]]; then test_disconnect; fi

        if test_diff; then final_exit_code=0; _harness_log "$test_name: PASS"; else _harness_log "$test_name: FAIL (Diffs)"; fi

        _harness_log "Archiving results from $(pwd) to $test_specific_archive_dir"
        # Archive all generated files, including raw and filtered .all files
        for f in expected.stdout expected.stderr expected.return \
                   actual.stdout actual.stderr actual.return \
                   expected.all.raw actual.all.raw \
                   expected.all actual.all \
                   results.diff; do
            if [[ -f "./$f" ]]; then
                if ! mv "./$f" "$test_specific_archive_dir/"; then
                     _harness_log "[ERROR] Failed mv ./$f to $test_specific_archive_dir/";
                fi;
            fi
        done
        echo "$([ "$final_exit_code" -eq 0 ] && echo "PASS" || echo "FAIL")" > "$test_specific_archive_dir/HARNESS_STATUS.txt"
        if [[ "${HARNESS_DEBUG:-0}" -ne 0 || $final_exit_code -ne 0 ]]; then _harness_log "[HARNESS_INFO] Test artifacts for $test_name in: $test_specific_archive_dir"; fi
        exit $final_exit_code
    )
    local test_exit_code=$?
    trap - EXIT SIGINT SIGTERM
    printf "FINISHED TEST: %s (%s)\n" "$test_name" "$([ "$test_exit_code" -eq 0 ] && echo "PASS" || echo "FAIL")"
    echo "-----------------------------------------------------"
    return $test_exit_code
}
export -f test_run_one
echo "[DEBUG_SOURCE] Defined test_run_one" >&2

test_run_all() {
    _harness_log "Starting all tests..."
    local overall_status=0; local tests_run=0; local tests_passed=0; local tests_failed=0
    local results_root_input="${HARNESS_RESULTS_ROOT_DIR:-./test_runs_archive}"
    local results_root_abs
    if [[ "$results_root_input" == /* ]]; then results_root_abs="$results_root_input"; else results_root_abs="$(pwd)/$results_root_input"; fi
    results_root_abs=$(realpath -m "$results_root_abs")
    mkdir -p "$results_root_abs" || { echo "[HARNESS_ERROR] CRITICAL: Cannot create results root: $results_root_abs (PWD: $(pwd))"; return 1; }
    _CURRENT_TEST_RUN_ARCHIVE_DIR=$(realpath -m "$results_root_abs/$(date +%Y-%m-%d_%H%M%S)")
    mkdir -p "$_CURRENT_TEST_RUN_ARCHIVE_DIR" || { echo "[HARNESS_ERROR] CRITICAL: Cannot create run archive: $_CURRENT_TEST_RUN_ARCHIVE_DIR"; return 1; }
    _harness_log "Archiving results for this run to: $_CURRENT_TEST_RUN_ARCHIVE_DIR"

    if [[ -z "$SIMPLE9P_BINARY" || ! -x "$SIMPLE9P_BINARY" ]]; then echo "[HARNESS_ERROR] SIMPLE9P_BINARY invalid." >&2; return 1; fi
    if [[ -z "$NINEP_FUSE_BINARY" || ! -x "$NINEP_FUSE_BINARY" ]]; then echo "[HARNESS_ERROR] NINEP_FUSE_BINARY invalid." >&2; return 1; fi
    local base_port="${HARNESS_PORT_BASE:-56400}"; _harness_log "Base port: $base_port."

    local test_case_dirs; mapfile -t test_case_dirs < <(test_list "." | sort)
    if [[ ${#test_case_dirs[@]} -eq 0 ]]; then echo "[HARNESS_INFO] No tests found in $(pwd)."; return 0; fi
    _harness_log "Found tests: ${test_case_dirs[*]}"

    for test_dir in "${test_case_dirs[@]}"; do
        tests_run=$((tests_run + 1)); local current_port_for_test=$((base_port + tests_run -1))
        if test_run_one "$test_dir" "$current_port_for_test" "$_CURRENT_TEST_RUN_ARCHIVE_DIR"; then
            tests_passed=$((tests_passed + 1)); else tests_failed=$((tests_failed + 1)); overall_status=1; fi
    done
    printf "=====================================================\nTEST SUMMARY\n"
    printf "All test results archived in: %s\n" "$_CURRENT_TEST_RUN_ARCHIVE_DIR"
    printf "Total tests run: %s\nPassed: %s\nFailed: %s\n" "$tests_run" "$tests_passed" "$tests_failed"
    printf "=====================================================\n"
    return $overall_status
}
export -f test_run_all
echo "[DEBUG_SOURCE] Defined test_run_all" >&2
echo "[DEBUG_SOURCE] Finished sourcing test_harness.sh" >&2
