#include "ui/tutorial/topics.h"

#include "generated/ui_strings.h"
#include "ui/tutorial/chains.h"

namespace sz::ui::tutorial {

const std::vector<Topic>& Topics() {
    static const std::vector<Topic> topics = {
        {kBasicsTopic, strings::kTutorialTopicsBasicsTitle, strings::kTutorialTopicsBasicsGist, &BasicsChain},
        {"pinning", strings::kTutorialTopicsPinningTitle, strings::kTutorialTopicsPinningGist, &PinningChain},
        {"drawing", strings::kTutorialTopicsDrawingTitle, strings::kTutorialTopicsDrawingGist, &DrawingChain},
    };
    return topics;
}

const Topic* FindTopic(std::string_view id) {
    for (const Topic& topic : Topics()) {
        if (topic.id == id) {
            return &topic;
        }
    }
    return nullptr;
}

}  // namespace sz::ui::tutorial
