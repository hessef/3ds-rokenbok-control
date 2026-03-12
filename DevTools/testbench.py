"""
Testbench to verify python code
"""
import os
import Controller

def clear_terminal():
    """
    Clears the terminal
    """

    if os.name == 'nt':
        #clear command for windows
        _ = os.system('cls') 
    else:
        #clear command for linux
        _ = os.system('clear')

def test_bool_to_bytes():
    """
    Test bool_array_to_bytes with a set of test cases
    """

    tests = 0
    failures = 0


    #test1
    tests += 1
    test1_arr = [True, False, True, False, False, True, False, True, True, False]
    test1_exp = bytes([0b10100101, 0b10000000])
    test1_res = Controller.bool_aray_to_bytes(test1_arr)
    if (test1_exp != test1_res):
        failures += 1

    #test2
    tests += 1
    test2_arr = [True, False, False, False, False, False, False, True, True, True, True, False, True]
    test2_exp = bytes([0b10000001, 0b11101000])
    test2_res = Controller.bool_aray_to_bytes(test2_arr)
    if (test2_exp != test2_res):
        failures += 1

    #print results and return number of failures
    print(f"[BOOL TO BYTE]: {tests - failures} TESTS PASSED OUT OF {tests}")

    return failures



def main():
    #clear terminal for cleanliness
    clear_terminal()

    failures = 0
    failures += test_bool_to_bytes()

    #print final results
    if failures == 0:
        print("ALL TESTS PASSED!")
    else:
        print(f"{failures} TESTS FAILED")

if __name__ == "__main__":
    main()