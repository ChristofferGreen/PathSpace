import os

# Hardcoded path to the software project
project_path = os.path.expanduser("~/workspace/PathSpace")

# Function to check if a file should be included
def should_include_file(file_path):
    excluded_dirs = ['.cache', 'build', '.vscode', 'myenv', 'scripts', '.git']
    return not any(dir in file_path for dir in excluded_dirs)

# Function to get file extension
def get_file_extension(file_path):
    return os.path.splitext(file_path)[1].lower()

# Function to write file contents
def write_file_contents(prompt_file, file_path):
    prompt_file.write(f"\n<file path=\"{file_path}\">\n")
    try:
        with open(file_path, 'r') as f:
            prompt_file.write(f.read())
    except Exception as e:
        prompt_file.write(f"Error reading file: {str(e)}")
    prompt_file.write("\n</file>\n")

# Open the prompt.txt file for writing
with open("prompt.txt", "w") as prompt_file:
    prompt_file.write("This prompt contains information about a software project. ")
    prompt_file.write("It includes a list of files and the contents of Markdown, C++, and CMake files.\n\n")

    # List of files
    prompt_file.write("List of files in the project:\n")
    for root, _, files in os.walk(project_path):
        for file in files:
            file_path = os.path.join(root, file)
            if should_include_file(file_path):
                prompt_file.write(f"{file_path}\n")

    # File contents
    prompt_file.write("\nContents of Markdown, C++, and CMake files:\n")
    
    # First, write Markdown files
    prompt_file.write("\nMarkdown Files:\n")
    for root, _, files in os.walk(project_path):
        for file in files:
            file_path = os.path.join(root, file)
            if should_include_file(file_path) and get_file_extension(file_path) == '.md':
                write_file_contents(prompt_file, file_path)

    # Then, write C++ files
    prompt_file.write("\nC++ Files:\n")
    for root, _, files in os.walk(project_path):
        for file in files:
            file_path = os.path.join(root, file)
            if should_include_file(file_path) and get_file_extension(file_path) in ['.cpp', '.hpp', '.h']:
                write_file_contents(prompt_file, file_path)

    # Finally, write CMake files
    prompt_file.write("\nCMake Files:\n")
    for root, _, files in os.walk(project_path):
        for file in files:
            file_path = os.path.join(root, file)
            if should_include_file(file_path) and (get_file_extension(file_path) == '.cmake' or file == 'CMakeLists.txt'):
                write_file_contents(prompt_file, file_path)

print("prompt.txt file has been created with the project information.")
