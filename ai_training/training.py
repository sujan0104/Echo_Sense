import numpy as np
import tensorflow as tf
from sklearn.model_selection import train_test_split
from sklearn.preprocessing import StandardScaler
from tensorflow.keras import layers, models

# ----------------------------
# 1. Simulated Dataset
# Replace this with real data later
# ----------------------------

def generate_data(samples=2000):
    X = []
    y = []

    for _ in range(samples):

        # Corridor (stable mid distances)
        corridor = np.random.normal(150, 10, 30)
        X.append(corridor)
        y.append(0)

        # Doorway (sudden spike in middle)
        doorway = np.random.normal(120, 15, 30)
        doorway[15:] += np.random.normal(100, 20)
        X.append(doorway)
        y.append(1)

        # Open Room (large fluctuating)
        open_room = np.random.normal(300, 50, 30)
        X.append(open_room)
        y.append(2)

        # Wall (short stable)
        wall = np.random.normal(40, 5, 30)
        X.append(wall)
        y.append(3)

        # Corner (gradual decrease)
        corner = np.linspace(200, 40, 30) + np.random.normal(0, 10, 30)
        X.append(corner)
        y.append(4)

    return np.array(X), np.array(y)


X, y = generate_data()

# Normalize
scaler = StandardScaler()
X = scaler.fit_transform(X)

# Train/test split
X_train, X_test, y_train, y_test = train_test_split(
    X, y, test_size=0.2, random_state=42
)

# ----------------------------
# 2. Build Tiny Neural Network
# ----------------------------

model = models.Sequential([
    layers.Input(shape=(30,)),
    layers.Dense(64, activation='relu'),
    layers.Dense(32, activation='relu'),
    layers.Dense(5, activation='softmax')
])

model.compile(
    optimizer='adam',
    loss='sparse_categorical_crossentropy',
    metrics=['accuracy']
)

# ----------------------------
# 3. Train
# ----------------------------

model.fit(
    X_train, y_train,
    epochs=25,
    batch_size=32,
    validation_data=(X_test, y_test)
)

# ----------------------------
# 4. Evaluate
# ----------------------------

loss, acc = model.evaluate(X_test, y_test)
print(f"Test Accuracy: {acc:.4f}")

# ----------------------------
# 5. Convert to TFLite
# ----------------------------

converter = tf.lite.TFLiteConverter.from_keras_model(model)
tflite_model = converter.convert()

with open("env_classifier.tflite", "wb") as f:
    f.write(tflite_model)

print("TFLite model exported as env_classifier.tflite")