# PathSpace

PathSpace is a new coordination language that draws inspiration from other coordination languages such as Linda, as well as concepts from reactive programming and data-oriented design. It provides a tuplespace-like system for managing typed hierarchical streams of data and computations.

## Introduction

PathSpace is designed to be a foundational component for larger systems, such as:
- Game engines
- Visualization engines
- Web-based book readers
- Any application requiring a tree-like structure for storing typed distributed data in a thread-safe manner

It's intended to be a multi-platform library supporting at least Windows, Linux, Android, macOS, and iOS.

## Key Features

- **Hierarchical Structure**: Similar to a file system, with PathSpaces as folders and Data as files
- **Typed Data Storage**: Supports various data types, including user-defined serializable classes and lambda functions
- **Polymorphism**: Allows user-supplied child classes to behave differently in different parts of the tree
- **Thread-Safe Operations**: All PathSpace operations are designed to be thread-safe
- **Glob Expressions**: Supports glob-style path matching for flexible data access
- **JSON Serialization**: Built-in support for JSON serialization and deserialization
- **Capability-based Access Control**: Fine-grained control over data access and operations

## Core Operations

1. **Insert**: Add data or a PathSpace to one or more paths
2. **Read**: Retrieve a copy of the value at a specified path
3. **Extract**: Similar to read, but removes the data from the PathSpace

## Advanced Features

- **Blocking Operations**: Wait for data to become available
- **Execution Management**: Support for lambda functions and delayed execution
- **Data Compression**: Automatic data compression and decompression
- **Out-of-core Storage**: Ability to store data on disk instead of RAM
- **Reactive Programming Support**: Enables creation of reactive data flows
- **Distributed Data Management**: Replication of spaces across multiple computers (planned)

## Use Cases

PathSpace can be used to implement various systems, including:
- Scene graphs for 3D renderers
- Vulkan renderers with easy mesh and shader management
- Just-in-time geometry generation systems
- Operating system-like environments with user and file system management
- Database front-ends