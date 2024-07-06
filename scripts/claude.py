import os
import anthropic

def get_project_structure(root_dir):
    """Get the structure of the project and contents of relevant files."""
    project_files = {}
    for root, _, files in os.walk(root_dir):
        for file in files:
            if file.endswith(('.cpp', '.hpp', '.h', '.txt', '.md', 'CMakeLists.txt')):
                full_path = os.path.join(root, file)
                relative_path = os.path.relpath(full_path, root_dir)
                project_files[relative_path] = None  # We'll read the content later
    return project_files

def read_file(file_path):
    """Read and return the contents of a file."""
    try:
        with open(file_path, 'r', encoding='utf-8') as file:
            return file.read()
    except UnicodeDecodeError:
        # If UTF-8 fails, try with ISO-8859-1
        with open(file_path, 'r', encoding='iso-8859-1') as file:
            return file.read()

def query_claude(prompt):
    api_key = os.environ.get('CLAUDE_API_KEY')
    if not api_key:
        raise ValueError("CLAUDE_API_KEY environment variable is not set")
    
    client = anthropic.Anthropic(api_key=api_key)
    response = client.completions.create(
        model="claude-2.1",
        max_tokens_to_sample=1000,
        prompt=f"\n\nHuman: {prompt}\n\nAssistant:",
    )
    return response.completion

project_root = os.path.expanduser("~/workspace/PathSpace")

def main():
    project_structure = get_project_structure(project_root)

    # Send over the paths of all relevant files
    file_paths = "\n".join(project_structure.keys())
    prompt = f"Here are the relevant files in the project:\n\n{file_paths}\n\nI will now send you the contents of these files."
    print(query_claude(prompt))

    # Send over the content of all files
    for file_path in project_structure.keys():
        full_path = os.path.join(project_root, file_path)
        content = read_file(full_path)
        file_prompt = f"""Here's the content of {file_path}:

<file path="{file_path}">
{content}
</file>

Please acknowledge that you've received this file."""
        print(query_claude(file_prompt))

    # User prompt loop
    while True:
        user_question = input("\nEnter your question about the project (or 'quit' to exit): ")
        if user_question.lower() == 'quit':
            break
        
        response = query_claude(user_question)
        print("\nClaude's response:")
        print(response)

if __name__ == "__main__":
    main()
