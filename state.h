#if !defined(STATE_H)
#define STATE_H

/**
 * Saves the program state to the file named in program_options.state_file.
 *
 * @returns 0 if the state was saved successfully, or if
 *          program_options.state_file is NULL.  Returns -1 if an error
 *          occurred.
*/
int save_state();

/**
 * Loads the program state from the file named in program_options.state_file.
 *
 * @returns LOAD_STATE_SUCCESS if a state file was present and loaded
 *          successfully,
 *          LOAD_STATE_FILE_NOT_SPECIFIED if program_options.state_file is set
 *          to NULL,
 *          LOAD_STATE_FILE_DOES_NOT_EXIST if the specified state file does not
 *          exist, or
 *          LOAD_STATE_LOAD_ERROR if an error occurred.
 */
int load_state();

#endif // !defined(STATE_H)
