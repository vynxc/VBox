#!/bin/bash

set -e  # Exit on any error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Function to print colored output
print_status() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Check if we're in a directory
if [ ! -d "$(pwd)" ]; then
    print_error "Current directory does not exist"
    exit 1
fi

# Get repository name (defaults to current directory name)
REPO_NAME="${1:-$(basename "$(pwd)")}"
REPO_DESCRIPTION="${2:-Created from existing folder}"
REPO_VISIBILITY="${3:-private}"  # private, public, or internal

print_status "Creating GitHub repository: $REPO_NAME"
print_status "Description: $REPO_DESCRIPTION"
print_status "Visibility: $REPO_VISIBILITY"

# Check if GitHub CLI is installed
if ! command -v gh &> /dev/null; then
    print_error "GitHub CLI (gh) is not installed"
    print_error "Install it from: https://cli.github.com/"
    exit 1
fi

# Check if user is authenticated with GitHub CLI
if ! gh auth status &> /dev/null; then
    print_error "Not authenticated with GitHub CLI"
    print_error "Run: gh auth login"
    exit 1
fi

# Check if git is installed
if ! command -v git &> /dev/null; then
    print_error "Git is not installed"
    exit 1
fi

# Check if already a git repository
if [ -d ".git" ]; then
    print_warning "Directory is already a git repository"
    
    # Check if there's already a remote origin
    if git remote get-url origin &> /dev/null; then
        CURRENT_REMOTE=$(git remote get-url origin)
        print_warning "Remote origin already exists: $CURRENT_REMOTE"
        
        read -p "Do you want to continue and update the remote? (y/N): " -n 1 -r
        echo
        if [[ ! $REPLY =~ ^[Yy]$ ]]; then
            print_status "Operation cancelled"
            exit 0
        fi
    fi
else
    # Initialize git repository
    print_status "Initializing git repository..."
    git init
    print_success "Git repository initialized"
fi

# Create .gitignore if it doesn't exist
if [ ! -f ".gitignore" ]; then
    print_status "Creating basic .gitignore..."
    cat > .gitignore << EOF
# OS generated files
.DS_Store
.DS_Store?
._*
.Spotlight-V100
.Trashes
ehthumbs.db
Thumbs.db

# IDE files
.vscode/
.idea/
*.swp
*.swo
*~

# Dependencies
node_modules/
vendor/

# Build outputs
dist/
build/
*.log
EOF
    print_success ".gitignore created"
fi

# Add all files to git
print_status "Adding files to git..."
git add .

# Check if there are any changes to commit
if git diff --staged --quiet; then
    print_warning "No changes to commit"
else
    # Commit changes
    print_status "Creating initial commit..."
    git commit -m "Initial commit"
    print_success "Initial commit created"
fi

# Create GitHub repository
print_status "Creating GitHub repository..."
if gh repo create "$REPO_NAME" --description "$REPO_DESCRIPTION" --"$REPO_VISIBILITY" --source=. --push; then
    print_success "GitHub repository created and code pushed!"
    
    # Get the repository URL
    REPO_URL=$(gh repo view --json url --jq '.url')
    print_success "Repository URL: $REPO_URL"
    
    # Open repository in browser (optional)
    read -p "Do you want to open the repository in your browser? (y/N): " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        gh repo view --web
    fi
else
    print_error "Failed to create GitHub repository"
    exit 1
fi

print_success "All done! Your folder is now a GitHub repository."
