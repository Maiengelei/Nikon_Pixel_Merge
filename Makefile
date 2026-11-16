CXX ?= c++
export MACOSX_DEPLOYMENT_TARGET ?= 13.0
CPPFLAGS += -Isrc
CXXFLAGS += -std=c++17 -O3 -Wall -Wextra -Wpedantic -pthread -MMD -MP
BUILD := build
CORE := $(addprefix $(BUILD)/,groups.o raw.o merge.o output.o job.o)
PRODUCT := Nikon_Pixel_Merge
BUNDLE := $(BUILD)/$(PRODUCT).app
APP := $(BUNDLE)/Contents
.PHONY: all app test clean
all: $(BUILD)/nikon-merge app
app: $(APP)/MacOS/$(PRODUCT) $(APP)/Info.plist $(APP)/Resources/AppIcon.icns
$(BUILD):
	mkdir -p "$@"
$(BUILD)/%.o: src/%.cpp Makefile | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c "$<" -o "$@"
$(BUILD)/App.o: app/App.mm Makefile | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fobjc-arc -c "$<" -o "$@"
$(BUILD)/nikon-merge: $(CORE) $(BUILD)/cli.o
	$(CXX) -pthread $^ -o "$@"
$(APP)/MacOS/$(PRODUCT): $(CORE) $(BUILD)/App.o
	mkdir -p "$(@D)"
	$(CXX) -pthread $^ -framework AppKit -framework UniformTypeIdentifiers -o "$@"
$(APP)/Info.plist: app/Info.plist
	mkdir -p "$(@D)"
	cp "$<" "$@"
$(APP)/Resources/AppIcon.icns: $(wildcard Resources/AppIcon.iconset/*.png)
	mkdir -p "$(@D)"
	iconutil -c icns Resources/AppIcon.iconset -o "$@"
$(BUILD)/compare: tools/compare.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) "$<" -o "$@"
clean:
	$(RM) -r "$(BUILD)"
-include $(wildcard $(BUILD)/*.d)

$(BUILD)/groups-test: tests/groups_test.cpp $(BUILD)/groups.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(BUILD)/groups.o -o "$@"
$(BUILD)/merge-test: tests/merge_test.cpp $(BUILD)/merge.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(BUILD)/merge.o -o "$@"
test: all $(BUILD)/groups-test $(BUILD)/merge-test $(BUILD)/compare
	$(BUILD)/merge-test
	$(BUILD)/groups-test $(if $(wildcard ImageFile/4Image),ImageFile)

# Standalone local distribution. Ad-hoc signing requires no developer account.
.PHONY: package
package: app
	codesign --force --options runtime --sign - "$(BUNDLE)"
	codesign --verify --strict "$(BUNDLE)"
	ditto -c -k --sequesterRsrc --keepParent "$(BUNDLE)" "$(BUILD)/$(PRODUCT)_macOS.zip"
